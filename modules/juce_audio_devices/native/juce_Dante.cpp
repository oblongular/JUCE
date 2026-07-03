namespace juce
{
namespace DanteClasses
{

static constexpr const char* kEndpointName      = "DanteEP";
static constexpr unsigned    kDefaultSampleRate  = 48000;
static constexpr unsigned    kDefaultNumChannels = 8;
static constexpr unsigned    kDefaultPeriodSize  = 64;
static constexpr unsigned    kDefaultTxLatencyUs = 1000;  // 1ms on Linux
static constexpr int         kInactiveTimeoutMs  = 1000;  // disconnect after 1s inactivity

static unsigned gTxLatencyUs = kDefaultTxLatencyUs;

static std::shared_ptr<Dante::IDanteLogger> makeSilentLogger()
{
    return std::make_shared<Dante::PrintfLogger> (Dante::LogLevel::NONE);
}

//==============================================================================
class DanteAudioIODevice final : public AudioIODevice,
                                  private Thread
{
public:
    DanteAudioIODevice (const String& deviceName,
                        unsigned initialNumInputs,
                        unsigned initialNumOutputs,
                        unsigned initialSampleRate,
                        unsigned initialPeriodSize,
                        unsigned txLatencyUs)
        : AudioIODevice (deviceName, "Dante"),
          // Linux's TASK_COMM_LEN is 16 bytes including the null terminator, so the
          // usable limit is 15 characters. pthread_setname_np() returns ERANGE (which
          // JUCE ignores) and leaves the name unchanged if given anything longer -
          // "JUCE/Dante-Audio" (16 chars) silently failed and left the thread showing
          // the inherited process name instead. Keep this at 15 characters or fewer.
          Thread ("JUCE/DanteAudio"),
          mContext (makeSilentLogger(), kInactiveTimeoutMs, false),
          mBufferView (mContext.getBufferView()),
          mAccessor (mBufferView, Dante::BlockAccessorConfig (txLatencyUs)),
          numInputs  (initialNumInputs),
          numOutputs (initialNumOutputs),
          sampleRate (initialSampleRate),
          periodSize (initialPeriodSize)
    {
        mContext.registerBlockAccessor (&mAccessor);
        allocateBuffers();
    }

    ~DanteAudioIODevice() override
    {
        close();
        mContext.deregisterBlockAccessor (&mAccessor);
    }

    //==============================================================================
    StringArray getOutputChannelNames() override
    {
        StringArray names;
        for (unsigned i = 1; i <= numOutputs; ++i)
            names.add ("Output " + String (i));
        return names;
    }

    StringArray getInputChannelNames() override
    {
        StringArray names;
        for (unsigned i = 1; i <= numInputs; ++i)
            names.add ("Input " + String (i));
        return names;
    }

    Array<double> getAvailableSampleRates() override  { return { (double) sampleRate }; }
    Array<int>    getAvailableBufferSizes() override  { return { (int) periodSize }; }
    int           getDefaultBufferSize()    override  { return (int) periodSize; }

    String open (const BigInteger& inputChannels,
                 const BigInteger& outputChannels,
                 double, int) override
    {
        activeInputChannels  = inputChannels;
        activeInputChannels .setRange ((int) numInputs,  256 - (int) numInputs,  false);
        activeOutputChannels = outputChannels;
        activeOutputChannels.setRange ((int) numOutputs, 256 - (int) numOutputs, false);
        deviceOpen = true;
        lastError  = {};
        return {};
    }

    void close() override
    {
        stop();
        deviceOpen = false;
    }

    bool   isOpen()       override  { return deviceOpen; }
    bool   isPlaying()    override  { return playing; }
    String getLastError() override  { return lastError; }

    void start (AudioIODeviceCallback* cb) override
    {
        cb->audioDeviceAboutToStart (this);
        callback.store (cb);
        playing = true;

        // startThread(Priority::highest) is a no-op on Linux — JUCE's own comment on
        // Thread::startThreadInternal() says non-realtime priority "is essentially
        // useless on Linux as only realtime has any options". startRealtimeThread()
        // is what actually applies a real-time scheduling policy before the thread
        // even starts running (pthread_attr_setschedpolicy, so there's no window
        // where it runs at normal priority first). On Linux this maps to SCHED_RR,
        // not SCHED_FIFO like DepApe/dep_sync_fanoutd — RR only differs from FIFO
        // when multiple threads share the same priority and need to time-slice,
        // which doesn't apply here as long as this thread runs on cores dedicated
        // to DSP-style consumers (see perf_tuning.dsp_cores in the NixOS module),
        // not DEP's own dep_cores/net_cores.
        //
        // Priority 8 of 0-10 maps to roughly raw SCHED_RR priority ~80 on Linux
        // (JUCE linearly maps 0-10 onto sched_get_priority_min/max(SCHED_RR),
        // typically 1-99), matching this project's dsp_rtprio convention (the
        // priority DSP clients like mc_delay/DepConvolver run at via rt-run-dsp.sh).
        if (! startRealtimeThread (Thread::RealtimeOptions()
                .withPriority (8)
                .withApproximateAudioProcessingTime ((int) periodSize, (double) sampleRate)))
        {
            // Unlike startThread(), a failed startRealtimeThread() leaves NO thread
            // running at all — createNativeThread() calls pthread_create() with the
            // realtime scheduling attributes already set, and if that pthread_create()
            // fails (most commonly EPERM: no CAP_SYS_NICE / RLIMIT_RTPRIO is 0 for this
            // user), it returns without falling back to a normal-priority thread. That
            // means silent, total audio failure (no processing loop runs) rather than
            // degraded performance, so this is worth surfacing loudly rather than
            // logging and continuing as if playback were still possible.
            lastError = "Failed to start Dante audio thread with realtime scheduling "
                        "(no CAP_SYS_NICE / RLIMIT_RTPRIO too low?) - audio will not run";
            Logger::writeToLog (lastError);
        }
    }

    void stop() override
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            waitForThreadToExit (5000);
        }

        if (auto* cb = callback.exchange (nullptr))
            cb->audioDeviceStopped();

        playing = false;
    }

    int    getCurrentBufferSizeSamples() override  { return (int) periodSize; }
    double getCurrentSampleRate()        override  { return (double) sampleRate; }
    int    getCurrentBitDepth()          override  { return 32; }

    BigInteger getActiveOutputChannels() const override  { return activeOutputChannels; }
    BigInteger getActiveInputChannels()  const override  { return activeInputChannels; }

    int getOutputLatencyInSamples() override  { return 0; }
    int getInputLatencyInSamples()  override  { return 0; }

private:
    //==============================================================================
    void run() override
    {
        while (! threadShouldExit())
        {
            if (mContext.connect (kEndpointName, false, 1) != 0)
                continue;

            runAudioLoop();
            mContext.disconnect();
        }
    }

    void runAudioLoop()
    {
        while (! threadShouldExit())
        {
            const auto result = mContext.wait();
            const auto& poll  = result.pollInfo;

            if (poll.mState == Dante::BufferView::State::UNAVAILABLE)
                return;

            if (poll.mReset)
            {
                reconfigureChannels (poll);
                continue;
            }

            handleLateErrors (result);

            const int txFrames = mAccessor.getTxFramesToWrite();
            if (txFrames <= 0)
                continue;

            auto* cb = callback.load();
            if (cb == nullptr)
                continue;

            // Cap to periodSize: after a late recovery mTxFramesToWrite = mTxLatencyFrames
            // (48 at 48 kHz) which exceeds periodSize (16), causing out-of-bounds writes
            // into the periodSize-allocated inputStorage/outputStorage buffers.
            const unsigned frames = std::min ((unsigned) txFrames, periodSize);

            readRxToFloat (frames);

            cb->audioDeviceIOCallbackWithContext (
                inputPtrs.data(),  (int) numInputs,
                outputPtrs.data(), (int) numOutputs,
                (int) frames, {});

            writeFloatToTx (frames);
        }
    }

    void allocateBuffers()
    {
        inputStorage.assign  (numInputs,  std::vector<float> (periodSize, 0.0f));
        outputStorage.assign (numOutputs, std::vector<float> (periodSize, 0.0f));
        inputPtrs.resize  (numInputs);
        outputPtrs.resize (numOutputs);
        for (unsigned i = 0; i < numInputs;  ++i)  inputPtrs[i]  = inputStorage[i].data();
        for (unsigned i = 0; i < numOutputs; ++i)  outputPtrs[i] = outputStorage[i].data();
    }

    void reconfigureChannels (const Dante::BufferView::PollInfo& poll)
    {
        mAccessor.setChannels (false);  // asymmetric: full TX and RX independently

        numInputs  = mBufferView.getRxChannelCount();
        numOutputs = mBufferView.getTxChannelCount();
        sampleRate = poll.mSamplerate;
        periodSize = mBufferView.getPeriodSizeFrames();

        allocateBuffers();

        activeInputChannels  = BigInteger();
        activeInputChannels .setRange (0, (int) numInputs,  true);
        activeOutputChannels = BigInteger();
        activeOutputChannels.setRange (0, (int) numOutputs, true);

        // Re-notify the callback so it can resize its internal buffers for the new geometry
        if (auto* cb = callback.load())
        {
            cb->audioDeviceStopped();
            cb->audioDeviceAboutToStart (this);
        }
    }

    void readRxToFloat (unsigned frames)
    {
        unsigned offset = 0;
        mAccessor.accessRxBlock ([&] (unsigned n, const std::vector<int32_t*>& rxPtrs)
        {
            for (unsigned ch = 0; ch < numInputs; ++ch)
                if (ch < rxPtrs.size() && rxPtrs[ch])
                    for (unsigned i = 0; i < n; ++i)
                        inputStorage[ch][offset + i] = rxPtrs[ch][i] * (1.0f / 2147483648.0f);
            offset += n;
            return n;
        }, frames);
    }

    void writeFloatToTx (unsigned frames)
    {
        unsigned offset = 0;
        mAccessor.accessTxBlock ([&] (unsigned n, const std::vector<int32_t*>& txPtrs)
        {
            for (unsigned ch = 0; ch < numOutputs; ++ch)
                if (ch < txPtrs.size() && txPtrs[ch])
                    for (unsigned i = 0; i < n; ++i)
                        txPtrs[ch][i] = (int32_t) (jlimit (-1.0f, 1.0f, outputStorage[ch][offset + i])
                                                    * 2147483647.0f);
            offset += n;
            return n;
        }, frames);
    }

    void handleLateErrors (const Dante::DefaultBufferContext::WaitResult& result)
    {
        if (! result.hasErrors())
            return;

        const int late = result.accessorLateValues[0].second;
        if (late < 0)
        {
            // Extremely late — reset both heads
            mAccessor.fastForwardTx ((unsigned) mAccessor.getTxFramesToWrite());
            mAccessor.fastForwardRx (mAccessor.getAvailRxFrames());
        }
        else
        {
            // Late by N frames — skip ahead to catch up
            mAccessor.fastForwardTx ((unsigned) late);
            mAccessor.fastForwardRx ((unsigned) late);
        }
    }

    //==============================================================================
    Dante::DefaultBufferContext mContext;
    Dante::BufferView&          mBufferView;
    Dante::BufferBlockAccessor  mAccessor;

    std::atomic<AudioIODeviceCallback*> callback { nullptr };

    std::vector<std::vector<float>> inputStorage, outputStorage;
    std::vector<float*>             inputPtrs, outputPtrs;

    unsigned numInputs,  numOutputs;
    unsigned sampleRate, periodSize;

    BigInteger activeInputChannels, activeOutputChannels;
    bool       deviceOpen = false;
    bool       playing    = false;
    String     lastError;
};

//==============================================================================
class DanteAudioIODeviceType final : public AudioIODeviceType
{
public:
    DanteAudioIODeviceType() : AudioIODeviceType ("Dante") {}

    void scanForDevices() override
    {
        Dante::DefaultBufferContext ctx (makeSilentLogger(), 2000, false);
        if (ctx.connect (kEndpointName, false, 0) != 0)
        {
            cachedName       = "Dante-Not-Present";
            cachedNumInputs  = 0;
            cachedNumOutputs = 0;
            return;
        }

        cachedName = kEndpointName;

        const auto result = ctx.wait();
        if (result.pollInfo.mState == Dante::BufferView::State::READY)
        {
            auto& view        = ctx.getBufferView();
            cachedNumInputs   = view.getRxChannelCount();
            cachedNumOutputs  = view.getTxChannelCount();
            cachedSampleRate  = view.getCurrentSamplerate();
            cachedPeriodSize  = view.getPeriodSizeFrames();
        }
        ctx.disconnect();
    }

    StringArray getDeviceNames (bool) const          override  { return { cachedName }; }
    int         getDefaultDeviceIndex (bool) const   override  { return 0; }
    bool        hasSeparateInputsAndOutputs() const  override  { return false; }

    int getIndexOfDevice (AudioIODevice* device, bool) const override
    {
        return device != nullptr ? 0 : -1;
    }

    AudioIODevice* createDevice (const String& outputName,
                                 const String& inputName) override
    {
        auto name = outputName.isNotEmpty() ? outputName : inputName;
        return new DanteAudioIODevice (name,
                                       cachedNumInputs,
                                       cachedNumOutputs,
                                       cachedSampleRate,
                                       cachedPeriodSize,
                                       gTxLatencyUs);
    }

private:
    String   cachedName       = kEndpointName;
    unsigned cachedNumInputs  = kDefaultNumChannels;
    unsigned cachedNumOutputs = kDefaultNumChannels;
    unsigned cachedSampleRate = kDefaultSampleRate;
    unsigned cachedPeriodSize = kDefaultPeriodSize;
};

} // namespace DanteClasses

void setDanteTxLatencyUs (unsigned microseconds) noexcept
{
    DanteClasses::gTxLatencyUs = microseconds;
}

} // namespace juce

namespace juce
{
namespace DanteClasses
{

static constexpr unsigned kDefaultSampleRate  = 48000;
static constexpr unsigned kDefaultNumChannels = 8;
static constexpr unsigned kDefaultPeriodSize  = 64;
static constexpr unsigned kDefaultTxLatencyUs = 1000;  // 1ms on Linux
static constexpr int      kInactiveTimeoutMs  = 1000;  // disconnect after 1s inactivity

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
          Thread ("Dante Audio"),
          mContext (makeSilentLogger(), kInactiveTimeoutMs, false),
          mBufferView (mContext.getBufferView()),
          mAccessor (mBufferView, Dante::BlockAccessorConfig (txLatencyUs)),
          numInputs  (initialNumInputs),
          numOutputs (initialNumOutputs),
          sampleRate (initialSampleRate),
          periodSize (initialPeriodSize)
    {
        mContext.registerBlockAccessor (&mAccessor);
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
        startThread (Thread::Priority::highest);
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
            if (mContext.connect ("DanteEP", false, 1) != 0)
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

            const unsigned frames = (unsigned) txFrames;

            readRxToFloat (frames);

            cb->audioDeviceIOCallbackWithContext (
                inputPtrs.data(),  (int) numInputs,
                outputPtrs.data(), (int) numOutputs,
                (int) frames, {});

            writeFloatToTx (frames);
        }
    }

    void reconfigureChannels (const Dante::BufferView::PollInfo& poll)
    {
        mAccessor.setChannels (false);  // asymmetric: full TX and RX independently

        numInputs  = mBufferView.getRxChannelCount();
        numOutputs = mBufferView.getTxChannelCount();
        sampleRate = poll.mSamplerate;
        periodSize = mBufferView.getPeriodSizeFrames();

        inputStorage.assign  (numInputs,  std::vector<float> (periodSize, 0.0f));
        outputStorage.assign (numOutputs, std::vector<float> (periodSize, 0.0f));

        inputPtrs.resize  (numInputs);
        outputPtrs.resize (numOutputs);
        for (unsigned i = 0; i < numInputs;  ++i)  inputPtrs[i]  = inputStorage[i].data();
        for (unsigned i = 0; i < numOutputs; ++i)  outputPtrs[i] = outputStorage[i].data();

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
        mAccessor.accessRxBlock ([&] (unsigned n, const std::vector<int32_t*>& rxPtrs)
        {
            for (unsigned ch = 0; ch < numInputs; ++ch)
                if (ch < rxPtrs.size() && rxPtrs[ch])
                    for (unsigned i = 0; i < n; ++i)
                        inputStorage[ch][i] = rxPtrs[ch][i] * (1.0f / 2147483648.0f);
            return n;
        }, frames);
    }

    void writeFloatToTx (unsigned frames)
    {
        mAccessor.accessTxBlock ([&] (unsigned n, const std::vector<int32_t*>& txPtrs)
        {
            for (unsigned ch = 0; ch < numOutputs; ++ch)
                if (ch < txPtrs.size() && txPtrs[ch])
                    for (unsigned i = 0; i < n; ++i)
                        txPtrs[ch][i] = (int32_t) (jlimit (-1.0f, 1.0f, outputStorage[ch][i])
                                                    * 2147483647.0f);
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
        if (ctx.connect ("DanteEP", false, 0) != 0)
        {
            cachedName       = "Dante-Not-Present";
            cachedNumInputs  = 0;
            cachedNumOutputs = 0;
            return;
        }

        cachedName = "Dante";

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
    String   cachedName       = "Dante-Not-Present";
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

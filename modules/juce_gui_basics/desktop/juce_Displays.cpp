/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

Displays::Displays (const Desktop& desktop)
{
    init (desktop);
}

void Displays::init (const Desktop& desktop)
{
    findDisplays (desktop);
    updateDeprecatedFields();
}

const Displays::Display* Displays::getDisplayForRect (Rectangle<int> rect, bool isPhysical) const noexcept
{
    int maxArea = -1;
    const Display* foundDisplay = nullptr;

    for (auto& display : displays)
    {
        const auto displayArea = (isPhysical ? display.physicalBounds : display.logicalBounds.toNearestInt()).getIntersection (rect);

        auto area = displayArea.getWidth() * displayArea.getHeight();

        if (area >= maxArea)
        {
            maxArea = area;
            foundDisplay = &display;
        }
    }

    return foundDisplay;
}

const Displays::Display* Displays::getDisplayForPoint (Point<int> point, bool isPhysical) const noexcept
{
    auto minDistance = std::numeric_limits<int>::max();
    const Display* foundDisplay = nullptr;

    for (auto& display : displays)
    {
        const auto displayArea = isPhysical ? display.physicalBounds : display.logicalBounds.toNearestInt();

        if (displayArea.contains (point))
            return &display;

        auto distance = displayArea.getCentre().getDistanceFrom (point);

        if (distance <= minDistance)
        {
            minDistance = distance;
            foundDisplay = &display;
        }
    }

    return foundDisplay;
}

Rectangle<int> Displays::physicalToLogical (Rectangle<int> rect, const Display* useScaleFactorOfDisplay) const noexcept
{
    return physicalToLogical (rect.toFloat(), useScaleFactorOfDisplay).toNearestInt();
}

Rectangle<float> Displays::physicalToLogical (Rectangle<float> rect, const Display* useScaleFactorOfDisplay) const noexcept
{
    const auto* display = useScaleFactorOfDisplay != nullptr ? useScaleFactorOfDisplay
                                                             : getDisplayForRect (rect.toNearestInt(), true);

    if (display == nullptr)
        return rect;

    return ((rect - display->physicalBounds.getTopLeft().toFloat()) / display->scale)
            + display->logicalBounds.getTopLeft();
}

Rectangle<int> Displays::logicalToPhysical (Rectangle<int> rect, const Display* useScaleFactorOfDisplay) const noexcept
{
    return logicalToPhysical (rect.toFloat(), useScaleFactorOfDisplay).toNearestInt();
}

Rectangle<float> Displays::logicalToPhysical (Rectangle<float> rect, const Display* useScaleFactorOfDisplay) const noexcept
{
    const auto* display = useScaleFactorOfDisplay != nullptr ? useScaleFactorOfDisplay
                                                             : getDisplayForRect (rect.toNearestInt(), false);

    if (display == nullptr)
        return rect;

    return ((rect.toFloat() - display->logicalBounds.getTopLeft()) * display->scale)
             + display->physicalBounds.getTopLeft().toFloat();
}

template <typename ValueType>
Point<ValueType> Displays::physicalToLogical (Point<ValueType> point, const Display* useScaleFactorOfDisplay) const noexcept
{
    const auto* display = useScaleFactorOfDisplay != nullptr ? useScaleFactorOfDisplay
                                                             : getDisplayForPoint (point.roundToInt(), true);

    if (display == nullptr)
        return point;

    const Point logicalTopLeft  (static_cast<ValueType> (display->logicalBounds.getX()),  static_cast<ValueType> (display->logicalBounds.getY()));
    const Point physicalTopLeft (static_cast<ValueType> (display->physicalBounds.getX()), static_cast<ValueType> (display->physicalBounds.getY()));

    return ((point - physicalTopLeft) / display->scale) + logicalTopLeft;
}

template <typename ValueType>
Point<ValueType> Displays::logicalToPhysical (Point<ValueType> point, const Display* useScaleFactorOfDisplay)  const noexcept
{
    const auto* display = useScaleFactorOfDisplay != nullptr ? useScaleFactorOfDisplay
                                                             : getDisplayForPoint (point.roundToInt(), false);

    if (display == nullptr)
        return point;

    const Point logicalTopLeft  (static_cast<ValueType> (display->logicalBounds.getX()),  static_cast<ValueType> (display->logicalBounds.getY()));
    const Point physicalTopLeft (static_cast<ValueType> (display->physicalBounds.getX()), static_cast<ValueType> (display->physicalBounds.getY()));

    return ((point - logicalTopLeft) * display->scale) + physicalTopLeft;
}

const Displays::Display* Displays::getPrimaryDisplay() const noexcept
{
    JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED

    const auto iter = std::find_if (displays.begin(), displays.end(), [] (auto& d) { return d.isMain; });
    return iter != displays.end() ? iter : nullptr;
}

RectangleList<int> Displays::getRectangleList (bool userAreasOnly) const
{
    JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED
    RectangleList<int> rl;

    for (auto& d : displays)
        rl.addWithoutMerging ((userAreasOnly ? d.userBounds : d.logicalBounds).toNearestInt());

    return rl;
}

Rectangle<int> Displays::getTotalBounds (bool userAreasOnly) const
{
    return getRectangleList (userAreasOnly).getBounds();
}

void Displays::refresh()
{
    Array<Display> oldDisplays;
    oldDisplays.swapWith (displays);

    init (Desktop::getInstance());

    if (oldDisplays != displays)
    {
        for (auto i = ComponentPeer::getNumPeers(); --i >= 0;)
            if (auto* peer = ComponentPeer::getPeer (i))
                peer->handleScreenSizeChange();
    }
}

static auto tie (const Displays::Display& d)
{
    JUCE_BEGIN_IGNORE_DEPRECATION_WARNINGS
    return std::tie (d.dpi,
                     d.isMain,
                     d.keyboardInsets,
                     d.safeAreaInsets,
                     d.scale,
                     d.topLeftPhysical,
                     d.totalArea,
                     d.userArea,
                     d.logicalBounds,
                     d.userBounds,
                     d.physicalBounds);
    JUCE_END_IGNORE_DEPRECATION_WARNINGS
}

static bool operator== (const Displays::Display& d1, const Displays::Display& d2) noexcept
{
    return tie (d1) == tie (d2);
}

//==============================================================================
// These methods are used for converting the totalArea and userArea Rectangles in Display from physical to logical
// pixels. We do this by constructing a graph of connected displays where the root node has position (0, 0); this can be
// safely converted to logical pixels using its scale factor and we can then traverse the graph and work out the logical pixels
// for all the other connected displays. We need to do this as the logical bounds of a display depend not only on its scale
// factor but also the scale factor of the displays connected to it.

/**
    Represents a node in our graph of displays.
*/
struct DisplayNode
{
    /** The Display object that this represents. */
    Displays::Display* display;

    /** True if this represents the 'root' display with position (0, 0). */
    bool isRoot = false;

    /** The parent node of this node in our display graph. This will have a correct logicalArea. */
    DisplayNode* parent = nullptr;

    /** The logical area to be calculated. This will be valid after processDisplay() has
        been called on this node.
    */
    Rectangle<double> logicalArea;
};

/** Recursive - will calculate and set the logicalArea member of current. */
static void processDisplay (DisplayNode* currentNode, Array<DisplayNode>& allNodes)
{
    const auto physicalArea = currentNode->display->logicalBounds.toDouble();
    const auto scale = currentNode->display->scale;

    if (! currentNode->isRoot)
    {
        const auto logicalWidth  = physicalArea.getWidth() / scale;
        const auto logicalHeight = physicalArea.getHeight() / scale;

        const auto physicalParentArea = currentNode->parent->display->logicalBounds.toDouble();
        const auto logicalParentArea  = currentNode->parent->logicalArea; // logical area of parent has already been calculated
        const auto parentScale        = currentNode->parent->display->scale;

        Rectangle<double> logicalArea (0.0, 0.0, logicalWidth, logicalHeight);

        if      (approximatelyEqual (physicalArea.getRight(), physicalParentArea.getX()))     logicalArea.setPosition ({ logicalParentArea.getX() - logicalWidth, physicalArea.getY() / parentScale });  // on left
        else if (approximatelyEqual (physicalArea.getX(), physicalParentArea.getRight()))     logicalArea.setPosition ({ logicalParentArea.getRight(),  physicalArea.getY() / parentScale });            // on right
        else if (approximatelyEqual (physicalArea.getBottom(), physicalParentArea.getY()))    logicalArea.setPosition ({ physicalArea.getX() / parentScale, logicalParentArea.getY() - logicalHeight }); // on top
        else if (approximatelyEqual (physicalArea.getY(), physicalParentArea.getBottom()))    logicalArea.setPosition ({ physicalArea.getX() / parentScale, logicalParentArea.getBottom() });            // on bottom
        else                                                               jassertfalse;

        currentNode->logicalArea = logicalArea;
    }
    else
    {
        // If currentNode is the root (position (0, 0)) then we can just scale the physical area
        currentNode->logicalArea = physicalArea / scale;
        currentNode->parent = currentNode;
    }

    // Find child nodes
    Array<DisplayNode*> children;
    for (auto& node : allNodes)
    {
        // Already calculated
        if (node.parent != nullptr)
            continue;

        const auto otherPhysicalArea = node.display->logicalBounds.toDouble();

        // If the displays are touching on any side
        if (approximatelyEqual (otherPhysicalArea.getX(), physicalArea.getRight())  || approximatelyEqual (otherPhysicalArea.getRight(),  physicalArea.getX())
         || approximatelyEqual (otherPhysicalArea.getY(), physicalArea.getBottom()) || approximatelyEqual (otherPhysicalArea.getBottom(), physicalArea.getY()))
        {
            node.parent = currentNode;
            children.add (&node);
        }
    }

    // Recursively process all child nodes
    for (auto child : children)
        processDisplay (child, allNodes);
}

void Displays::updateDeprecatedFields()
{
    for (auto& display : displays)
    {
        JUCE_BEGIN_IGNORE_DEPRECATION_WARNINGS
        display.topLeftPhysical = display.physicalBounds.getTopLeft();
        display.totalArea       = display.logicalBounds.toNearestInt();
        display.userArea        = display.userBounds.toNearestInt();
        JUCE_END_IGNORE_DEPRECATION_WARNINGS
    }
}

/** This is called when the displays Array has been filled out with the info for all connected displays and the
    logicalBounds and userBounds Rectangles need to be converted from physical to logical coordinates.
*/
void Displays::updateToLogical()
{
    if (displays.size() == 1)
    {
        auto& display = displays.getReference (0);

        display.logicalBounds = (display.logicalBounds.toDouble() / display.scale).toFloat();
        display.userBounds    = (display.userBounds   .toDouble() / display.scale).toFloat();

        return;
    }

    Array<DisplayNode> displayNodes;

    for (auto& d : displays)
    {
        DisplayNode node;

        node.display = &d;

        if (d.logicalBounds.getTopLeft().isOrigin())
            node.isRoot = true;

        displayNodes.add (node);
    }

    auto* root = [&displayNodes]() -> DisplayNode*
    {
        for (auto& node : displayNodes)
            if (node.isRoot)
                return &node;

        auto minDistance = std::numeric_limits<float>::max();
        DisplayNode* retVal = nullptr;

        for (auto& node : displayNodes)
        {
            auto distance = node.display->logicalBounds.getTopLeft().getDistanceFrom ({});

            if (distance < minDistance)
            {
                minDistance = distance;
                retVal = &node;
            }
        }

        if (retVal != nullptr)
            retVal->isRoot = true;

        return retVal;
    }();

    // Must have a root node!
    jassert (root != nullptr);

    // Recursively traverse the display graph from the root and work out logical bounds
    processDisplay (root, displayNodes);

    for (auto& node : displayNodes)
    {
        // All of the nodes should have a parent
        jassert (node.parent != nullptr);

        const auto unscaledUserArea = (node.display->userBounds - node.display->logicalBounds.getTopLeft());
        const auto relativeUserArea = unscaledUserArea.toDouble() / node.display->scale;

        node.display->logicalBounds = node.logicalArea.toFloat();
        node.display->userBounds = (relativeUserArea + node.logicalArea.getTopLeft()).toFloat();
    }
}

/** @cond */
// explicit template instantiations
template Point<int>   Displays::physicalToLogical (Point<int>,   const Display*) const noexcept;
template Point<float> Displays::physicalToLogical (Point<float>, const Display*) const noexcept;

template Point<int>   Displays::logicalToPhysical (Point<int>,   const Display*) const noexcept;
template Point<float> Displays::logicalToPhysical (Point<float>, const Display*) const noexcept;
/** @endcond */

//==============================================================================
// Deprecated methods
const Displays::Display& Displays::getDisplayContaining (Point<int> position) const noexcept
{
    JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED
    const auto* best = &displays.getReference (0);
    auto bestDistance = std::numeric_limits<float>::max();

    for (auto& d : displays)
    {
        if (d.logicalBounds.contains (position.toFloat()))
        {
            best = &d;
            break;
        }

        auto distance = d.logicalBounds.getCentre().getDistanceFrom (position.toFloat());

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = &d;
        }
    }

    return *best;
}

const Displays::Display& Displays::findDisplayForRect (Rectangle<int> rect, bool isPhysical) const noexcept
{
    if (auto* display = getDisplayForRect (rect, isPhysical))
        return *display;

    return emptyDisplay;
}

const Displays::Display& Displays::findDisplayForPoint (Point<int> point, bool isPhysical) const noexcept
{
    if (auto* display = getDisplayForPoint (point, isPhysical))
        return *display;

    return emptyDisplay;
}

const Displays::Display& Displays::getMainDisplay() const noexcept
{
    if (auto* display = getPrimaryDisplay())
        return *display;

    return emptyDisplay;
}

} // namespace juce

#include "uiwidget.h"
#include "uimanager.h"
#include "uianchorlayout.h"
#include "uiverticallayout.h"

#include <framework/core/eventdispatcher.h>
#include <framework/graphics/image.h>
#include <framework/graphics/borderimage.h>
#include <framework/graphics/fontmanager.h>
#include <framework/otml/otmlnode.h>
#include <framework/graphics/graphics.h>
#include <framework/platform/platform.h>

UIWidget::UIWidget()
{
    m_updateEventScheduled = false;
    m_states = DefaultState;

    // generate an unique id, this is need because anchored layouts find widgets by id
    static unsigned long id = 1;
    m_id = fw::mkstr("widget", id++);
}

UIWidget::~UIWidget()
{
    // clear all references
    releaseLuaFieldsTable();
    m_focusedChild.reset();
    m_layout.reset();
    m_parent.reset();
    m_lockedChildren.clear();
    m_children.clear();
}

void UIWidget::setup()
{
    setVisible(true);
    setEnabled(true);
    setFocusable(true);
    setPressed(false);
    setSizeFixed(false);
    setFont(g_fonts.getDefaultFont());
    setBackgroundColor(Color::white);
    setForegroundColor(Color::white);
    setOpacity(255);
    setMarginTop(0);
    setMarginRight(0);
    setMarginBottom(0);
    setMarginLeft(0);
}

void UIWidget::destroy()
{
    // remove itself from parent
    if(UIWidgetPtr parent = getParent())
        parent->removeChild(asUIWidget());
}

void UIWidget::render()
{
    // draw background
    if(m_image) {
        g_graphics.bindColor(m_backgroundColor);
        m_image->draw(m_rect);
    }

    // draw children
    for(const UIWidgetPtr& child : m_children) {
        // render only visible children with a valid rect
        if(child->isExplicitlyVisible() && child->getRect().isValid()) {
            // store current graphics opacity
            int oldOpacity = g_graphics.getOpacity();

            // decrease to self opacity
            if(child->getOpacity() < oldOpacity)
                g_graphics.setOpacity(child->getOpacity());

            child->render();

            // debug draw box
            //g_graphics.bindColor(Color::green);
            //g_graphics.drawBoundingRect(child->getRect());
            //g_fonts.getDefaultFont()->renderText(child->getId(), child->getPosition() + Point(2, 0), Color::red);

            g_graphics.setOpacity(oldOpacity);
        }
    }
}

void UIWidget::setStyle(const std::string& styleName)
{
    OTMLNodePtr styleNode = g_ui.getStyle(styleName);
    applyStyle(styleNode);
    m_style = styleNode;
}

void UIWidget::setStyleFromNode(const OTMLNodePtr& styleNode)
{
    applyStyle(styleNode);
    m_style = styleNode;
}

void UIWidget::setParent(const UIWidgetPtr& parent)
{
    UIWidgetPtr self = asUIWidget();

    // remove from old parent
    UIWidgetPtr oldParent = getParent();
    if(oldParent && oldParent->hasChild(self))
        oldParent->removeChild(self);

    // reset parent
    m_parent.reset();

    // set new parent
    if(parent) {
        m_parent = parent;

        // add to parent if needed
        if(!parent->hasChild(self))
            parent->addChild(self);
    }
}

void UIWidget::setRect(const Rect& rect)
{
    // only update if the rect really changed
    Rect oldRect = m_rect;
    if(rect == oldRect)
        return;

    m_rect = rect;

    // updates own layout
    updateLayout();

    // avoid massive update events
    if(!m_updateEventScheduled) {
        UIWidgetPtr self = asUIWidget();
        g_dispatcher.addEvent([self, oldRect]() {
            self->m_updateEventScheduled = false;
            self->onGeometryUpdate(oldRect, self->getRect());
        });
    }
    m_updateEventScheduled = true;
}

bool UIWidget::isVisible()
{
    if(!m_visible)
        return false;
    else if(UIWidgetPtr parent = getParent())
        return parent->isVisible();
    else
        return asUIWidget() == g_ui.getRootWidget();
}

bool UIWidget::hasChild(const UIWidgetPtr& child)
{
    auto it = std::find(m_children.begin(), m_children.end(), child);
    if(it != m_children.end())
        return true;
    return false;
}

UIWidgetPtr UIWidget::getRootParent()
{
    if(UIWidgetPtr parent = getParent())
        return parent->getRootParent();
    else
        return asUIWidget();
}

UIWidgetPtr UIWidget::getChildAfter(const UIWidgetPtr& relativeChild)
{
    auto it = std::find(m_children.begin(), m_children.end(), relativeChild);
    if(it != m_children.end() && ++it != m_children.end())
        return *it;
    return nullptr;
}

UIWidgetPtr UIWidget::getChildBefore(const UIWidgetPtr& relativeChild)
{
    auto it = std::find(m_children.rbegin(), m_children.rend(), relativeChild);
    if(it != m_children.rend() && ++it != m_children.rend())
        return *it;
    return nullptr;
}

UIWidgetPtr UIWidget::getChildById(const std::string& childId)
{
    for(const UIWidgetPtr& child : m_children) {
        if(child->getId() == childId)
            return child;
    }
    return nullptr;
}

UIWidgetPtr UIWidget::getChildByPos(const Point& childPos)
{
    for(auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        const UIWidgetPtr& widget = (*it);
        if(widget->isExplicitlyVisible() && widget->getRect().contains(childPos))
            return widget;
    }

    return nullptr;
}

UIWidgetPtr UIWidget::getChildByIndex(int index)
{
    index = index <= 0 ? (m_children.size() + index) : index-1;
    if(index >= 0 && (uint)index < m_children.size())
        return m_children.at(index);
    return nullptr;
}

UIWidgetPtr UIWidget::recursiveGetChildById(const std::string& id)
{
    UIWidgetPtr widget = getChildById(id);
    if(!widget) {
        for(const UIWidgetPtr& child : m_children) {
            widget = child->recursiveGetChildById(id);
            if(widget)
                break;
        }
    }
    return widget;
}

UIWidgetPtr UIWidget::recursiveGetChildByPos(const Point& childPos)
{
    for(const UIWidgetPtr& child : m_children) {
        if(child->getRect().contains(childPos)) {
            if(UIWidgetPtr subChild = child->recursiveGetChildByPos(childPos))
                return subChild;
            return child;
        }
    }
    return nullptr;
}

UIWidgetPtr UIWidget::backwardsGetWidgetById(const std::string& id)
{
    UIWidgetPtr widget = getChildById(id);
    if(!widget) {
        if(UIWidgetPtr parent = getParent())
            widget = parent->backwardsGetWidgetById(id);
    }
    return widget;
}

void UIWidget::focusChild(const UIWidgetPtr& child, FocusReason reason)
{
    if(child && !hasChild(child)) {
        logError("attempt to focus an unknown child in a UIWidget");
        return;
    }

    if(child != m_focusedChild) {
        UIWidgetPtr oldFocused = m_focusedChild;
        m_focusedChild = child;

        if(child) {
            child->setLastFocusReason(reason);
            child->updateState(FocusState);
            child->updateState(ActiveState);
        }

        if(oldFocused) {
            oldFocused->setLastFocusReason(reason);
            oldFocused->updateState(FocusState);
            oldFocused->updateState(ActiveState);
        }
    }
}

void UIWidget::addChild(const UIWidgetPtr& child)
{
    if(!child) {
        logWarning("attempt to add a null child into a UIWidget");
        return;
    }

    if(hasChild(child)) {
        logWarning("attempt to add a child again into a UIWidget");
        return;
    }

    m_children.push_back(child);
    child->setParent(asUIWidget());

    // always focus new child
    if(child->isFocusable() && child->isExplicitlyVisible() && child->isExplicitlyEnabled())
        focusChild(child, ActiveFocusReason);

    // create default layout
    if(!m_layout)
        m_layout = UILayoutPtr(new UIAnchorLayout(asUIWidget()));

    // add to layout and updates it
    m_layout->addWidget(child);

    // update new child states
    child->updateStates();
}

void UIWidget::insertChild(int index, const UIWidgetPtr& child)
{
    if(!child) {
        logWarning("attempt to insert a null child into a UIWidget");
        return;
    }

    if(hasChild(child)) {
        logWarning("attempt to insert a child again into a UIWidget");
        return;
    }

    index = index <= 0 ? (m_children.size() + index) : index-1;

    assert(index >= 0 && (uint)index <= m_children.size());

    // retrieve child by index
    auto it = m_children.begin() + index;
    m_children.insert(it, child);
    child->setParent(asUIWidget());

    // create default layout if needed
    if(!m_layout)
        m_layout = UILayoutPtr(new UIAnchorLayout(asUIWidget()));

    // add to layout and updates it
    m_layout->addWidget(child);

    // update new child states
    child->updateStates();
}

void UIWidget::removeChild(const UIWidgetPtr& child)
{
    // remove from children list
    auto it = std::find(m_children.begin(), m_children.end(), child);
    if(it != m_children.end()) {
        // defocus if needed
        if(m_focusedChild == child)
            focusChild(nullptr, ActiveFocusReason);

        // unlock child if it was locked
        unlockChild(child);

        m_children.erase(it);

        // reset child parent
        assert(child->getParent() == asUIWidget());
        child->setParent(nullptr);

        m_layout->removeWidget(child);

        // update child states
        child->updateStates();
    } else
        logError("attempt to remove an unknown child from a UIWidget");
}

void UIWidget::focusNextChild(FocusReason reason)
{
    UIWidgetPtr toFocus;
    UIWidgetList rotatedChildren(m_children);

    if(m_focusedChild) {
        auto focusedIt = std::find(rotatedChildren.begin(), rotatedChildren.end(), m_focusedChild);
        if(focusedIt != rotatedChildren.end()) {
            std::rotate(rotatedChildren.begin(), focusedIt, rotatedChildren.end());
            rotatedChildren.pop_front();
        }
    }

    // finds next child to focus
    for(const UIWidgetPtr& child : rotatedChildren) {
        if(child->isFocusable()) {
            toFocus = child;
            break;
        }
    }

    if(toFocus)
        focusChild(toFocus, reason);
}

void UIWidget::moveChildToTop(const UIWidgetPtr& child)
{
    if(!child)
        return;

    // remove and push child again
    auto it = std::find(m_children.begin(), m_children.end(), child);
    assert(it != m_children.end());
    m_children.erase(it);
    m_children.push_back(child);
}

void UIWidget::lockChild(const UIWidgetPtr& child)
{
    if(!child)
        return;

    assert(hasChild(child));

    // prevent double locks
    unlockChild(child);

    // disable all other children
    for(const UIWidgetPtr& otherChild : m_children) {
        if(otherChild == child)
            child->setEnabled(true);
        else
            otherChild->setEnabled(false);
    }

    m_lockedChildren.push_front(child);

    // lock child focus
    if(child->isFocusable())
      focusChild(child, ActiveFocusReason);

    moveChildToTop(child);
}

void UIWidget::unlockChild(const UIWidgetPtr& child)
{
    if(!child)
        return;

    assert(hasChild(child));

    auto it = std::find(m_lockedChildren.begin(), m_lockedChildren.end(), child);
    if(it == m_lockedChildren.end())
        return;

    m_lockedChildren.erase(it);

    // find new chick to lock
    UIWidgetPtr lockedChild;
    if(m_lockedChildren.size() > 0)
        lockedChild = m_lockedChildren.front();

    for(const UIWidgetPtr& otherChild : m_children) {
        // lock new child
        if(lockedChild) {
            if(otherChild == lockedChild)
                lockedChild->setEnabled(true);
            else
                otherChild->setEnabled(false);
        }
        // else unlock all
        else
            otherChild->setEnabled(true);
    }
}

void UIWidget::updateParentLayout()
{
    if(UIWidgetPtr parent = getParent())
        parent->updateLayout();
    else
        updateLayout();
}

void UIWidget::updateLayout()
{
    if(m_layout)
        m_layout->update();
}

void UIWidget::updateState(WidgetState state)
{
    bool newStatus = true;
    bool oldStatus = hasState(state);
    bool updateChildren = false;

    if(state == ActiveState) {
        UIWidgetPtr widget = asUIWidget();
        UIWidgetPtr parent;
        do {
            parent = widget->getParent();
            if(!widget->isExplicitlyEnabled() ||
               ((parent && parent->getFocusedChild() != widget))) {
                newStatus = false;
                break;
            }
        } while(widget = parent);

        updateChildren = true;
    }
    else if(state == FocusState) {
        newStatus = (getParent() && getParent()->getFocusedChild() == asUIWidget());
    }
    else if(state == HoverState) {
        updateChildren = true;
        Point mousePos = g_platform.getMouseCursorPos();
        UIWidgetPtr widget = asUIWidget();
        UIWidgetPtr parent;
        do {
            parent = widget->getParent();
            if(!widget->getRect().contains(mousePos) ||
               (parent && widget != parent->getChildByPos(mousePos))) {
                newStatus = false;
                break;
            }
        } while(widget = parent);
    }
    else if(state == PressedState) {
        newStatus = m_pressed;
    }
    else if(state == DisabledState) {
        updateChildren = true;
        UIWidgetPtr widget = asUIWidget();
        do {
            if(!widget->isExplicitlyEnabled()) {
                newStatus = false;
                break;
            }
        } while(widget = widget->getParent());
    }
    else {
        return;
    }

    if(updateChildren) {
        for(const UIWidgetPtr& child : m_children)
            child->updateState(state);
    }

    if(newStatus != oldStatus) {
        if(newStatus)
            m_states |= state;
        else
            m_states &= ~state;

        updateStyle();

        if(state == FocusState)
            onFocusChange(newStatus, m_lastFocusReason);
        else if(state == HoverState)
            onHoverChange(newStatus);
    }
}

void UIWidget::updateStates()
{
    updateState(ActiveState);
    updateState(FocusState);
    updateState(DisabledState);
    updateState(HoverState);
}

void UIWidget::updateStyle()
{
    if(!m_style)
        return;

    OTMLNodePtr newStateStyle = OTMLNode::create();

    // copy only the changed styles from default style
    if(m_stateStyle) {
        for(OTMLNodePtr node : m_stateStyle->children()) {
            if(OTMLNodePtr otherNode = m_style->get(node->tag()))
                newStateStyle->addChild(otherNode->clone());
        }
    }

    // merge states styles, NOTE: order does matter
    OTMLNodePtr style = m_style->get("state.active");
    if(style && hasState(ActiveState))
        newStateStyle->merge(style);

    style = m_style->get("state.focus");
    if(style && hasState(FocusState))
        newStateStyle->merge(style);

    style = m_style->get("state.hover");
    if(style && hasState(HoverState))
        newStateStyle->merge(style);

    style = m_style->get("state.pressed");
    if(style && hasState(PressedState))
        newStateStyle->merge(style);

    style = m_style->get("state.disabled");
    if(style && hasState(DisabledState))
        newStateStyle->merge(style);

    applyStyle(newStateStyle);
    m_stateStyle = newStateStyle;
}

void UIWidget::applyStyle(const OTMLNodePtr& styleNode)
{
    try {
        onStyleApply(styleNode);
    } catch(std::exception& e) {
        logError("failed to apply widget '", m_id, "' style: ", e.what());
    }
}

void UIWidget::onStyleApply(const OTMLNodePtr& styleNode)
{
    // first set id
    if(const OTMLNodePtr& node = styleNode->get("id"))
        setId(node->value());

    // load styles used by all widgets
    for(const OTMLNodePtr& node : styleNode->children()) {
        // background image
        if(node->tag() == "image") {
            setImage(Image::loadFromOTML(node));
        }
        else if(node->tag() == "border-image") {
            setImage(BorderImage::loadFromOTML(node));
        }
        // font
        else if(node->tag() == "font") {
            setFont(g_fonts.getFont(node->value()));
        }
        // foreground color
        else if(node->tag() == "color") {
            setForegroundColor(node->value<Color>());
        }
        // background color
        else if(node->tag() == "background-color") {
            setBackgroundColor(node->value<Color>());
        }
        // opacity
        else if(node->tag() == "opacity") {
            setOpacity(node->value<int>());
        }
        // focusable
        else if(node->tag() == "focusable") {
            setFocusable(node->value<bool>());
        }
        // size
        else if(node->tag() == "size") {
            resize(node->value<Size>());
        }
        else if(node->tag() == "width") {
            setWidth(node->value<int>());
        }
        else if(node->tag() == "height") {
            setHeight(node->value<int>());
        }
        else if(node->tag() == "size fixed") {
            setSizeFixed(node->value<bool>());
        }
        // absolute position
        else if(node->tag() == "position") {
            moveTo(node->value<Point>());
        }
        else if(node->tag() == "x") {
            setX(node->value<int>());
        }
        else if(node->tag() == "y") {
            setY(node->value<int>());
        }
        // margins
        else if(node->tag() == "margin.left") {
            setMarginLeft(node->value<int>());
        }
        else if(node->tag() == "margin.right") {
            setMarginRight(node->value<int>());
        }
        else if(node->tag() == "margin.top") {
            setMarginTop(node->value<int>());
        }
        else if(node->tag() == "margin.bottom") {
            setMarginBottom(node->value<int>());
        }
        // layouts
        else if(node->tag() == "layout") {
            // layout is set only once
            assert(!m_layout);
            if(node->value() == "verticalBox") {
                setLayout(UILayoutPtr(new UIVerticalLayout(asUIWidget())));
            } else if(node->value() == "anchor") {
                setLayout(UILayoutPtr(new UIAnchorLayout(asUIWidget())));
            }
        }
        // anchors
        else if(boost::starts_with(node->tag(), "anchors.")) {
            UIWidgetPtr parent = getParent();
            if(!parent)
                throw OTMLException(node, "cannot create anchor, there is no parent widget!");

            UIAnchorLayoutPtr anchorLayout = parent->getLayout()->asUIAnchorLayout();
            if(!anchorLayout)
                throw OTMLException(node, "cannot create anchor, the parent widget doesn't use anchor layout!");

            std::string what = node->tag().substr(8);
            if(what == "fill") {
                anchorLayout->fill(asUIWidget(), node->value());
            } else if(what == "centerIn") {
                anchorLayout->centerIn(asUIWidget(), node->value());
            } else {
                AnchorEdge anchoredEdge = fw::translateAnchorEdge(what);

                std::string anchorDescription = node->value();
                std::vector<std::string> split;
                boost::split(split, anchorDescription, boost::is_any_of(std::string(".")));
                if(split.size() != 2)
                    throw OTMLException(node, "invalid anchor description");

                std::string hookedWidgetId = split[0];
                AnchorEdge hookedEdge = fw::translateAnchorEdge(split[1]);

                if(anchoredEdge == AnchorNone)
                    throw OTMLException(node, "invalid anchor edge");

                if(hookedEdge == AnchorNone)
                    throw OTMLException(node, "invalid anchor target edge");

                anchorLayout->addAnchor(asUIWidget(), anchoredEdge, hookedWidgetId, hookedEdge);
            }
        }
    }
}

void UIWidget::onGeometryUpdate(const Rect& oldRect, const Rect& newRect)
{

}

void UIWidget::onFocusChange(bool focused, FocusReason reason)
{

}

void UIWidget::onHoverChange(bool hovered)
{

}

bool UIWidget::onKeyPress(uchar keyCode, char keyChar, int keyboardModifiers)
{
    // do a backup of children list, because it may change while looping it
    UIWidgetList children;
    for(const UIWidgetPtr& child : m_children) {
        // events on hidden or disabled widgets are discarded
        if(!child->isExplicitlyEnabled() || !child->isExplicitlyVisible())
            continue;

        // key events go only to containers or focused child
        if(child->isFocused())
            children.push_back(child);
    }

    for(const UIWidgetPtr& child : children) {
        if(child->onKeyPress(keyCode, keyChar, keyboardModifiers))
            return true;
    }

    return false;
}

bool UIWidget::onKeyRelease(uchar keyCode, char keyChar, int keyboardModifiers)
{
    // do a backup of children list, because it may change while looping it
    UIWidgetList children;
    for(const UIWidgetPtr& child : m_children) {
        // events on hidden or disabled widgets are discarded
        if(!child->isExplicitlyEnabled() || !child->isExplicitlyVisible())
            continue;

        // key events go only to focused child
        if(child->isFocused())
            children.push_back(child);
    }

    for(const UIWidgetPtr& child : children) {
        if(child->onKeyRelease(keyCode, keyChar, keyboardModifiers))
            return true;
    }

    return false;
}

bool UIWidget::onMousePress(const Point& mousePos, MouseButton button)
{
    // do a backup of children list, because it may change while looping it
    UIWidgetList children;
    for(const UIWidgetPtr& child : m_children) {
        // events on hidden or disabled widgets are discarded
        if(!child->isExplicitlyEnabled() || !child->isExplicitlyVisible())
            continue;

        // mouse press events only go to children that contains the mouse position
        if(child->getRect().contains(mousePos) && child == getChildByPos(mousePos))
            children.push_back(child);
    }

    for(const UIWidgetPtr& child : children) {
        // when a focusable item is focused it must gain focus
        if(child->isFocusable())
            focusChild(child, MouseFocusReason);

        bool mustEnd = child->onMousePress(mousePos, button);

        if(!child->getChildByPos(mousePos) && !child->isPressed())
            child->setPressed(true);

        if(mustEnd)
            return true;
    }

    return false;
}

bool UIWidget::onMouseRelease(const Point& mousePos, MouseButton button)
{
    // do a backup of children list, because it may change while looping it
    UIWidgetList children;
    for(const UIWidgetPtr& child : m_children) {
        // events on hidden or disabled widgets are discarded
        if(!child->isExplicitlyEnabled() || !child->isExplicitlyVisible())
            continue;

        // mouse release events go to all children
        children.push_back(child);
    }

    for(const UIWidgetPtr& child : children) {
        bool mustEnd = child->onMouseRelease(mousePos, button);

        if(child->isPressed())
            child->setPressed(false);

        if(mustEnd)
            return true;
    }

    return false;
}

bool UIWidget::onMouseMove(const Point& mousePos, const Point& mouseMoved)
{
    // do a backup of children list, because it may change while looping it
    UIWidgetList children;
    for(const UIWidgetPtr& child : m_children) {
        // events on hidden or disabled widgets are discarded
        if(!child->isExplicitlyEnabled() || !child->isExplicitlyVisible())
            continue;

        // mouse move events go to all children
        children.push_back(child);
    }

    for(const UIWidgetPtr& child : children) {
        if(child->onMouseMove(mousePos, mouseMoved))
            return true;
    }

    return false;
}

bool UIWidget::onMouseWheel(const Point& mousePos, MouseWheelDirection direction)
{
    // do a backup of children list, because it may change while looping it
    UIWidgetList children;
    for(const UIWidgetPtr& child : m_children) {
        // events on hidden or disabled widgets are discarded
        if(!child->isExplicitlyEnabled() || !child->isExplicitlyVisible())
            continue;

        // mouse wheel events only go to children that contains the mouse position
        if(child->getRect().contains(mousePos) && child == getChildByPos(mousePos))
            children.push_back(child);
    }

    for(const UIWidgetPtr& child : children) {
        if(child->onMouseWheel(mousePos, direction))
            return true;
    }

    return false;
}

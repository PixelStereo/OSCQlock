#pragma once
// A drawer on the right side of a window: a narrow handle that is always there, and a panel that slides out of it. The window around it keeps a
// fixed size at each of the two positions (give its layout QLayout::SetFixedSize: it follows the drawer while it moves).
#include <QWidget>

#include <functional>

class QToolButton;
class QVariantAnimation;

class Drawer : public QWidget
{
public:
    // `content` is shown at `width` when the drawer is open; `tip` says what the handle opens
    Drawer(QWidget *content, int width, const QString &tip, QWidget *parent = nullptr);

    bool isOpen() const { return m_open; }
    // Opens or closes it (no `toggled` call back: whoever asked knows)
    void setOpen(bool open, bool animate = true);
    // Told when the user opens or closes it with the handle
    void onToggled(std::function<void(bool)> callback) { m_toggled = std::move(callback); }

private:
    void show(int width);

    QToolButton *m_handle;
    QWidget *m_clip;
    QWidget *m_content;
    QVariantAnimation *m_animation;
    int m_width;
    bool m_open = false;
    std::function<void(bool)> m_toggled;
};

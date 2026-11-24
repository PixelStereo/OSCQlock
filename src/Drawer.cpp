#include "Drawer.h"

#include <QEasingCurve>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVariantAnimation>

namespace {

// Holds the content at its full width and shows as much of it as the drawer is open: the content does not reflow while it slides
class Clip : public QWidget
{
public:
    Clip(QWidget *content, int width, QWidget *parent) : QWidget(parent), m_content(content), m_width(width)
    {
        content->setParent(this);
        setMinimumWidth(0);
    }

protected:
    void resizeEvent(QResizeEvent *) override { m_content->setGeometry(0, 0, m_width, height()); }

private:
    QWidget *m_content;
    int m_width;
};

} // namespace

Drawer::Drawer(QWidget *content, int width, const QString &tip, QWidget *parent) : QWidget(parent), m_content(content), m_width(width)
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    m_handle = new QToolButton;
    m_handle->setCheckable(true);
    m_handle->setAutoRaise(true);
    m_handle->setText(QString::fromUtf8("◂"));
    m_handle->setToolTip(tip);
    m_handle->setFixedWidth(22);
    m_handle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    row->addWidget(m_handle);

    m_clip = new Clip(content, width, this);
    m_clip->setFixedWidth(0);
    m_clip->hide();
    row->addWidget(m_clip);

    m_animation = new QVariantAnimation(this);
    m_animation->setDuration(180);
    m_animation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { show(v.toInt()); });

    connect(m_handle, &QToolButton::toggled, this, [this](bool on) {
        setOpen(on);
        if (m_toggled) m_toggled(on);
    });
}

void Drawer::show(int width)
{
    m_clip->setFixedWidth(width);
    m_clip->setVisible(width > 0);
}

void Drawer::setOpen(bool open, bool animate)
{
    m_open = open;
    {
        const QSignalBlocker block(m_handle);
        m_handle->setChecked(open);
    }
    m_handle->setText(open ? QString::fromUtf8("▸") : QString::fromUtf8("◂"));
    const int to = open ? m_width : 0;
    m_animation->stop();
    if (!animate) {
        show(to);
        return;
    }
    m_animation->setStartValue(m_clip->width());
    m_animation->setEndValue(to);
    m_animation->start();
}

#include "mainwindow.h"

#include <QVBoxLayout>
#include <QWindow>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);

    // -- Status area --
    m_statusLabel = new QLabel("isActive: false");
    m_statusLabel->setStyleSheet("font-size: 14px; font-weight: bold; padding: 8px; background: #2b2b2b; color: #aaa; border-radius: 4px;");
    layout->addWidget(m_statusLabel);

    m_envTokenLabel = new QLabel("XDG_ACTIVATION_TOKEN: (not set)");
    m_envTokenLabel->setWordWrap(true);
    m_envTokenLabel->setStyleSheet("font-family: monospace; padding: 8px; background: #1e1e1e; color: #888; border-radius: 4px;");
    layout->addWidget(m_envTokenLabel);

    // -- Buttons --
    auto *btnActivate = new QPushButton("Request Activate (5s delay)");
    auto *btnAlert = new QPushButton("Set Alert (5s delay)");
    auto *btnClearAlert = new QPushButton("Clear Alert");
    auto *btnSpawn = new QPushButton("Spawn Second Window (activate after 3s)");

    layout->addWidget(btnActivate);
    layout->addWidget(btnAlert);
    layout->addWidget(btnClearAlert);
    layout->addWidget(btnSpawn);
    layout->addStretch();

    setCentralWidget(central);
    resize(360, 280);

    connect(btnActivate, &QPushButton::clicked, this, &MainWindow::onRequestActivate);
    connect(btnAlert, &QPushButton::clicked, this, &MainWindow::onSetAlert);
    connect(btnClearAlert, &QPushButton::clicked, this, &MainWindow::onClearAlert);
    connect(btnSpawn, &QPushButton::clicked, this, &MainWindow::onSpawnWindow);

    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::updateStatus);
    timer->start(500);
}

void MainWindow::onRequestActivate()
{
    // 延迟 5s 触发，让用户有时间切走焦点再观察激活行为
    // QWindow::requestActivate() → 携带 serial 的 token 请求
    QTimer::singleShot(5000, this, [this]() {
        if (auto *w = windowHandle())
            w->requestActivate();
    });
}

void MainWindow::onSetAlert()
{
    // 延迟 5s 触发，让用户有时间切走焦点再观察 alert 行为
    // QWindow::alert() → 无 serial 的 token 请求 → compositor 任务栏提示
    QTimer::singleShot(5000, this, [this]() {
        if (auto *w = windowHandle())
            w->alert(0);
    });
}

void MainWindow::onClearAlert()
{
    if (auto *w = windowHandle())
        w->alert(-1);
}

void MainWindow::onSpawnWindow()
{
    auto *second = new QMainWindow;
    second->setAttribute(Qt::WA_DeleteOnClose);
    second->setWindowTitle("Activation Demo (Secondary)");
    second->resize(320, 200);

    auto *label = new QLabel("I will requestActivate() in 3 seconds...\nSwitch focus away from me to see the effect.");
    label->setAlignment(Qt::AlignCenter);
    second->setCentralWidget(label);
    second->show();

    QTimer::singleShot(3000, second, [second]() {
        if (second->windowHandle())
            second->windowHandle()->requestActivate();
    });
}

void MainWindow::updateStatus()
{
    auto *w = windowHandle();
    if (!w)
        return;

    bool active = w->isActive();
    m_statusLabel->setText(QStringLiteral("isActive: %1").arg(active ? "true" : "false"));
    m_statusLabel->setStyleSheet(
        active
            ? "font-size: 14px; font-weight: bold; padding: 8px; background: #1b5e20; color: #a5d6a7; border-radius: 4px;"
            : "font-size: 14px; font-weight: bold; padding: 8px; background: #2b2b2b; color: #aaa; border-radius: 4px;");

    auto token = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
    m_envTokenLabel->setText(QStringLiteral("XDG_ACTIVATION_TOKEN: %1").arg(token.isEmpty() ? "(not set)" : token));
}

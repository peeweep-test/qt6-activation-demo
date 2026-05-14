#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QTimer>

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onRequestActivate();
    void onSetAlert();
    void onClearAlert();
    void onSpawnWindow();
    void updateStatus();

private:
    QLabel *m_statusLabel;
    QLabel *m_envTokenLabel;
};

#endif // MAINWINDOW_H

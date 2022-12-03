#ifndef MAINPICKER_H
#define MAINPICKER_H

#include <QMainWindow>
#include <QObject>
#include <QEvent>

QT_BEGIN_NAMESPACE
namespace Ui { class MainPicker; }
QT_END_NAMESPACE

class MainPicker : public QMainWindow
{
    Q_OBJECT

public:
    MainPicker(QWidget *parent = nullptr);
    ~MainPicker();

    void onMonitorButtonClicked(QObject* target, QEvent* event);

private:
    Ui::MainPicker *ui;
};
#endif // MAINPICKER_H

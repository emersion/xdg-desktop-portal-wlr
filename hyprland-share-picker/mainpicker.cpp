#include "mainpicker.h"
#include "./ui_mainpicker.h"
#include <QDebug>

MainPicker::MainPicker(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainPicker)
{
    ui->setupUi(this);
}

MainPicker::~MainPicker()
{
    delete ui;
}

void MainPicker::onMonitorButtonClicked(QObject* target, QEvent* event) {
    qDebug() << "click";
}

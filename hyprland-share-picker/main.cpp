#include "mainpicker.h"

#include <QApplication>
#include <QScreen>
#include <QWidget>
#include <QTabWidget>
#include <QPushButton>
#include <QtWidgets>
#include <QtDebug>
#include <QObject>
#include <QEvent>
#include <string>
#include <iostream>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>

std::string execAndGet(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

QApplication* pickerPtr = nullptr;

int main(int argc, char *argv[]) {
    qputenv("QT_LOGGING_RULES", "qml=false");
    QApplication picker(argc, argv);
    pickerPtr = &picker;
    MainPicker w;

    // get the tabwidget
    const auto TABWIDGET = (QTabWidget*)w.children()[1]->children()[0];

    const auto TAB1 = (QWidget*)TABWIDGET->children()[0];

    const auto SCREENS_SCROLL_AREA_CONTENTS = (QWidget*)TAB1->findChild<QWidget*>("screens")->findChild<QScrollArea*>("scrollArea")->findChild<QWidget*>("scrollAreaWidgetContents");

    // add all screens
    const auto SCREENS = picker.screens();

    constexpr int BUTTON_WIDTH  = 441;
    constexpr int BUTTON_HEIGHT = 41;
    constexpr int BUTTON_PAD    = 4;

    for (int i = 0; i < SCREENS.size(); ++i) {
        const auto GEOMETRY = SCREENS[i]->geometry();

        QString text = QString::fromStdString(std::string("Screen " + std::to_string(i) + " at " + std::to_string(GEOMETRY.x()) + ", "
                            + std::to_string(GEOMETRY.y()) + " (" + std::to_string(GEOMETRY.width()) + "x"
                            + std::to_string(GEOMETRY.height()) + ") (") + SCREENS[i]->name().toStdString() + ")");
        QPushButton* button = new QPushButton(text, (QWidget*)SCREENS_SCROLL_AREA_CONTENTS);
        button->move(9, 5 + (BUTTON_HEIGHT + BUTTON_PAD) * i);
        button->resize(BUTTON_WIDTH, BUTTON_HEIGHT);
        QObject::connect(button, &QPushButton::clicked, [=] () {
            std::string ID = button->text().toStdString();
            ID = ID.substr(ID.find_last_of('(') + 1);
            ID = ID.substr(0, ID.find_last_of(')'));

            std::cout << "screen:" << ID << "\n";
            pickerPtr->quit();
            return 0;
        });
    }

    SCREENS_SCROLL_AREA_CONTENTS->resize(SCREENS_SCROLL_AREA_CONTENTS->size().width(), 5 + (BUTTON_HEIGHT + BUTTON_PAD) * SCREENS.size());

    // windows
    const auto WINDOWS_SCROLL_AREA_CONTENTS = (QWidget*)TAB1->findChild<QWidget*>("windows")->findChild<QScrollArea*>("scrollArea_2")->findChild<QWidget*>("scrollAreaWidgetContents_2");

    // get all windows from hyprctl
    std::string windowsList = execAndGet("hyprctl clients");

    // loop over them
    int windowIterator = 0;
    while (windowsList.find("Window ") != std::string::npos) {
        auto windowPropLen = windowsList.find("Window ", windowsList.find("\n\n") + 2);
        if (windowPropLen == std::string::npos)
            windowPropLen = windowsList.length();
        const std::string windowProp = windowsList.substr(0, windowPropLen);
        windowsList = windowsList.substr(windowPropLen);

        // get window name
        auto windowName = windowProp.substr(windowProp.find(" -> ") + 4);
        windowName = windowName.substr(0, windowName.find_first_of('\n') - 1);

        auto windowHandle = windowProp.substr(7, windowProp.find(" -> ") - 7);

        QString text = QString::fromStdString("Window " + windowHandle + ": " + windowName);

        QPushButton* button = new QPushButton(text, (QWidget*)WINDOWS_SCROLL_AREA_CONTENTS);
        button->move(9, 5 + (BUTTON_HEIGHT + BUTTON_PAD) * windowIterator);
        button->resize(BUTTON_WIDTH, BUTTON_HEIGHT);
        QObject::connect(button, &QPushButton::clicked, [=] () {
            std::string HANDLE = button->text().toStdString();
            HANDLE = HANDLE.substr(7, HANDLE.find_first_of(':') - 7);

            std::cout << "window:" << HANDLE << "\n";
            pickerPtr->quit();
            return 0;
        });

        windowIterator++;
    }

    WINDOWS_SCROLL_AREA_CONTENTS->resize(WINDOWS_SCROLL_AREA_CONTENTS->size().width(), 5 + (BUTTON_HEIGHT + BUTTON_PAD) * windowIterator);

    // lastly, region
    const auto REGION_OBJECT = (QWidget*)TAB1->findChild<QWidget*>("region");

    QString text = "Select region...";

    QPushButton* button = new QPushButton(text, (QWidget*)REGION_OBJECT);
    button->move(79, 80);
    button->resize(321, 41);
    QObject::connect(button, &QPushButton::clicked, [=] () {
        auto REGION = execAndGet("slurp -f \"%o %x %y %w %h\"");
        REGION = REGION.substr(0, REGION.length() - 1);

        // now, get the screen
        QScreen* pScreen = nullptr;
        if (REGION.find_first_of(' ') == std::string::npos) {
            std::cout << "error1\n";
            pickerPtr->quit();
            return 1;
        }
        const auto SCREEN_NAME = REGION.substr(0, REGION.find_first_of(' '));

        for (auto& screen : SCREENS) {
            if (screen->name().toStdString() == SCREEN_NAME) {
                pScreen = screen;
                break;
            }
        }

        if (!pScreen) {
            std::cout << "error2\n";
            pickerPtr->quit();
            return 1;
        }

        // get all the coords
        try {
            REGION = REGION.substr(REGION.find_first_of(' ') + 1);
            const auto X = std::stoi(REGION.substr(0, REGION.find_first_of(' ')));
            REGION = REGION.substr(REGION.find_first_of(' ') + 1);
            const auto Y = std::stoi(REGION.substr(0, REGION.find_first_of(' ')));
            REGION = REGION.substr(REGION.find_first_of(' ') + 1);
            const auto W = std::stoi(REGION.substr(0, REGION.find_first_of(' ')));
            REGION = REGION.substr(REGION.find_first_of(' ') + 1);
            const auto H = std::stoi(REGION);

            std::cout << "region:" << SCREEN_NAME << "@" << X - pScreen->geometry().x() << "," << Y - pScreen->geometry().y() << "," << W << "," << H << "\n";
            pickerPtr->quit();
            return 0;
        } catch (...) {
            std::cout << "error3\n";
            pickerPtr->quit();
            return 1;
        }

        std::cout << "error4\n";
        pickerPtr->quit();
        return 1;
    });

    w.show();
    return picker.exec();
}

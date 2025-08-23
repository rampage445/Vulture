QT       += core gui widgets concurrent sql
VERSION = 1.0

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17
LIBS += -lshell32 -luser32 -lgdi32
# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    drivewatcher.cpp \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    drivewatcher.h \
    mainwindow.h \
    traverselib.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    resources.qrc

FORMS += \
    mainwindow.ui

RC_FILE = app_icon.rc

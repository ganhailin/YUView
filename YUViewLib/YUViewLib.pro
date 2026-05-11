QT += core gui widgets opengl xml concurrent network

equals(QT_MAJOR_VERSION, 6) {
    QT += openglwidgets
}

TEMPLATE = lib
CONFIG += staticlib
CONFIG += c++17
CONFIG -= debug_and_release
CONFIG += object_parallel_to_source

SOURCES += $$files(src/*.cpp, true)
HEADERS += $$files(src/*.h, true)

macx {
    # Include Objective-C++ source files for macOS-specific functionality
    SOURCES += $$files(src/*.mm, true)
}

FORMS += $$files(ui/*.ui, false)

INCLUDEPATH += src/

RESOURCES += \
    images/images.qrc \
    docs/docs.qrc \
    shaders/shaders.qrc

contains(QT_ARCH, x86_32|i386) {
    warning("You are building for a 32 bit system. This is untested and not supported.")
}

SVNN = $$system("git describe --tags")
LASTHASH = $$system("git rev-parse HEAD")
isEmpty(LASTHASH) {
    LASTHASH = 0
}
isEmpty(SVNN) {
    SVNN = 0
}

win32 {
    DEFINES += NOMINMAX
}

win32-msvc* {
    HASHSTRING = '\\"$${LASTHASH}\\"'
    DEFINES += YUVIEW_HASH=$${HASHSTRING}
}
win32-g++ | linux | macx {
    HASHSTRING = '\\"$${LASTHASH}\\"'
    DEFINES += YUVIEW_HASH=\"$${HASHSTRING}\"
}

macx {
    # Link against Cocoa and OpenGL frameworks for EDR support
    LIBS += -framework Cocoa -framework OpenGL
    # Ensure .mm files are compiled as Objective-C++
    QMAKE_CXXFLAGS += -x objective-c++
}

VERSTR = '\\"$${SVNN}\\"'
DEFINES += YUVIEW_VERSION=$${VERSTR}

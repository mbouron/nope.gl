if (NOT DEFINED ENV{NGL_ANDROID_ENV})
    message(FATAL_ERROR "NGL_ANDROID_ENV must set")
endif()
set(NGL_ANDROID_ENV $ENV{NGL_ANDROID_ENV}/${ANDROID_ABI})
set(NGL_LIB_PATH ${NGL_ANDROID_ENV}/lib)
set(NGL_HEADER_PATH ${NGL_ANDROID_ENV}/include)

cmake_minimum_required(VERSION 3.22.1)

project("nopegl_native")

add_library(${CMAKE_PROJECT_NAME} SHARED
        nopegl.c
)

include_directories(${NGL_HEADER_PATH})

set(NGL_LIBS
        nopegl
        nopemd
        avfilter
        swresample
        swscale
        avformat
        avcodec
        avutil
        avdevice
        png16
        freetype
        fribidi
        harfbuzz
        lcms2
        )

foreach(LIB ${NGL_LIBS})
    add_library(${LIB} SHARED IMPORTED)
    set_target_properties(${LIB} PROPERTIES IMPORTED_LOCATION ${NGL_LIB_PATH}/lib${LIB}.so)
endforeach()

target_link_libraries(${CMAKE_PROJECT_NAME}
        log
        android
        mediandk
        EGL
        GLESv2
        vulkan
        m
        c++_shared
        ${NGL_LIBS}
        )

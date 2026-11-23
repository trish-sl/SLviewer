# -*- cmake -*-

project(llrender)

include(00-Common)
include(OpenGL)
include(FreeType)
include(LLCommon)
include(LLImage)
include(LLWindow)

set(llrender_SOURCE_FILES
    fsglretire.cpp # <FS:Trish/> bindless handles released behind GPU fences
    fsglstream.cpp # <FS:Trish/> FSRenderThread
    fsgputimers.cpp # <FS:Trish/> FSRenderGPUTimings
    fsreversez.cpp # <FS:Trish/> FSRenderReverseZ
    fstaajitter.cpp # <FS:Trish/> FSRenderTAA
    fstextureupload.cpp # <FS:Trish/> FSPerfTextureUploadPBO
    llcubemap.cpp
    llcubemaparray.cpp
    llfontbitmapcache.cpp
    llfontfreetype.cpp
    llfontfreetypesvg.cpp
    llfontgl.cpp
    llfontvertexbuffer.cpp
    llfontregistry.cpp
    llgl.cpp
    llglslshader.cpp
    llgltexture.cpp
    llimagegl.cpp
    llrender.cpp
    llrender2dutils.cpp
    llrendernavprim.cpp
    llrendersphere.cpp
    llrendertarget.cpp
    llshadermgr.cpp
    lltexture.cpp
    lltexturemanagerbridge.cpp
    lluiimage.cpp
    llvertexbuffer.cpp
    llglcommonfunc.cpp
    )
    
set(llrender_HEADER_FILES
    fsglretire.h # <FS:Trish/> bindless handles released behind GPU fences
    fsglstream.h # <FS:Trish/> FSRenderThread
    fsgputimers.h # <FS:Trish/> FSRenderGPUTimings
    fsreversez.h # <FS:Trish/> FSRenderReverseZ
    fstaajitter.h # <FS:Trish/> FSRenderTAA
    fsglstream_funcs.inl # <FS:Trish/> FSRenderThread
    fstextureupload.h # <FS:Trish/> FSPerfTextureUploadPBO
    CMakeLists.txt

    llcubemap.h
    llcubemaparray.h
    llfontgl.h
    llfontvertexbuffer.h
    llfontfreetype.h
    llfontfreetypesvg.h
    llfontbitmapcache.h
    llfontregistry.h
    llgl.h
    llglheaders.h
    llglslshader.h
    llglstates.h
    llgltexture.h
    llgltypes.h
    llimagegl.h
    llrender.h
    llrender2dutils.h
    llrendernavprim.h
    llrendersphere.h
    llshadermgr.h
    lltexture.h
    lltexturemanagerbridge.h
    lluiimage.h
    lluiimage.inl
    llvertexbuffer.h
    llglcommonfunc.h
    )

list(APPEND llrender_SOURCE_FILES ${llrender_HEADER_FILES})

if (BUILD_HEADLESS)
  add_library (llrenderheadless
    ${llrender_SOURCE_FILES}
    )
  target_include_directories(llrenderheadless INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries(llrenderheadless
          llcommon
          llimage
          llmath
          llxml
          llfilesystem
          llwindowheadless
          ll::freetype
          OpenGL::GL
          )

  if (USE_PRECOMPILED_HEADERS)
    target_precompile_headers(llrenderheadless REUSE_FROM llprecompiled)
  endif()
endif (BUILD_HEADLESS)

add_library (llrender ${llrender_SOURCE_FILES})
target_include_directories(llrender INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

set_property(TARGET llrender PROPERTY POSITION_INDEPENDENT_CODE ON)

# Libraries on which this library depends, needed for Linux builds
# Sort by high-level to low-level
target_link_libraries(llrender
        llcommon
        llimage
        llmath
        llfilesystem
        llxml
        llwindow
        ll::freetype
        )

if (NOT USE_SDL_WINDOW)
target_link_libraries(llrender
        OpenGL::GL
        )

endif()

if (USE_PRECOMPILED_HEADERS)
 target_precompile_headers(llrender REUSE_FROM llprecompiled)
endif ()

if (LL_TESTS)
  include(LLAddBuildTest)
  LL_ADD_INTEGRATION_TEST(llgltexture "" "llrender")
  # <FS:Beq> Context-free regression tests for pooled-buffer binding and layout.
  if (LINUX AND USE_SDL_WINDOW)
    LL_ADD_INTEGRATION_TEST(llvertexbuffer_pool "" "llrender")
  endif ()
  # </FS:Beq>
endif ()


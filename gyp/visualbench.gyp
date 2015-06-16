# Copyright 2015 Google Inc.
#
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
# GYP file to build visual bench tool
{
  'includes': [
    'apptype_console.gypi',
  ],
  'targets': [
    {
      'target_name': 'visualbench',
      'type': 'executable',
      'mac_bundle' : 1,
      'include_dirs' : [
        '../include/gpu',
        '../src/core',
        '../src/images',
      ],
      'sources': [
        '../tools/VisualBench.h',
        '../tools/VisualBench.cpp',
      ],
      'dependencies': [
        'flags.gyp:flags_common',
        'skia_lib.gyp:skia_lib',
        'tools.gyp:proc_stats',
        'tools.gyp:timer',
        'views.gyp:views',
      ],
      'conditions' : [
        [ 'skia_os == "win"', {
          'sources' : [
            '../src/views/win/SkOSWindow_Win.cpp',
            '../src/views/win/skia_win.cpp',
          ],
        }],
        [ 'skia_os == "mac"', {
          'sources': [
            '../example/mac/HelloWorldNSView.mm',
            '../example/mac/HelloWorldDelegate.mm',

            '../src/views/mac/SkEventNotifier.mm',
            '../src/views/mac/skia_mac.mm',
            '../src/views/mac/SkNSView.mm',
            '../src/views/mac/SkOptionsTableView.mm',
            '../src/views/mac/SkOSWindow_Mac.mm',
            '../src/views/mac/SkTextFieldCell.m',
          ],
          'include_dirs' : [
            '../src/views/mac/'
          ],
          'xcode_settings' : {
            'INFOPLIST_FILE' : '../example/mac/HelloWorld-Info.plist',
          },
          'mac_bundle_resources' : [
            '../example/mac/HelloWorld.xib'
          ],
        }],
        [ 'skia_os == "android"', {
          'dependencies': [
            'android_deps.gyp:Android_VisualBench',
            'android_deps.gyp:native_app_glue',
          ],
         'link_settings': {
            'libraries': [
              '-landroid',
              '-lGLESv2',
              '-lEGL',
            ],
          },        
        }],
      ],
    },
  ],
}

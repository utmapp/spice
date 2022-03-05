test_lib_include = [spice_server_include, include_directories('.')]
test_lib_deps = [spice_server_deps]
if host_machine.system() != 'windows'
  test_lib_deps += [dependency('gio-unix-2.0')]
endif

test_lib_sources = [
  'basic-event-loop.c',
  'basic-event-loop.h',
  'test-display-base.c',
  'test-display-base.h',
  'test-glib-compat.h',
  'win-alarm.c',
  'win-alarm.h',
  'vmc-emu.c',
  'vmc-emu.h',
]

test_libs = []
test_libs += static_library('testlib', test_lib_sources,
                            link_with: spice_server_static_lib,
                            include_directories : test_lib_include,
                            dependencies : test_lib_deps,
                            install : false)

stat_test_libs = {'testlib_stat1' : ['1', '0', '0'],
                  'testlib_stat2' : ['2', '0', '1'],
                  'testlib_stat3' : ['3', '1', '0'],
                  'testlib_stat4' : ['4', '1', '1']}
foreach lib, params : stat_test_libs
  test_libs += static_library(lib, 'stat-test.c',
                              c_args : ['-DTEST_NAME=stat_test@0@'.format(params[0]),
                                        '-DTEST_COMPRESS_STAT=@0@'.format(params[1]),
                                        '-DTEST_RED_WORKER_STAT=@0@'.format(params[2])],
                              include_directories : test_lib_include,
                              dependencies : test_lib_deps,
                              install : false)
endforeach

tests = [
  ['test-codecs-parsing', true],
  ['test-options', true],
  ['test-stat', true],
  ['test-agent-msg-filter', true],
  ['test-loop', true],
  ['test-qxl-parsing', true],
  ['test-leaks', true],
  ['test-vdagent', true],
  ['test-fail-on-null-core-interface', true],
  ['test-empty-success', true],
  ['test-channel', true],
  ['test-stream-device', true],
  ['test-listen', true],
  ['test-record', true],
  ['test-display-no-ssl', false],
  ['test-display-streaming', false],
  ['test-playback', false],
  ['test-display-resolution-changes', false],
  ['test-two-servers', false],
  ['test-display-width-stride', false],
]

if spice_server_has_sasl
  tests += [['test-sasl', true]]
endif

if spice_server_has_smartcard == true
  tests += [['test-smartcard', true]]
endif

if host_machine.system() != 'windows'
  tests += [
    ['test-stream', true],
    ['test-stat-file', true],
  ]
  if host_machine.system() not in ['darwin', 'ios']
    tests += [
      ['test-websocket', false],
    ]
  endif
endif

if spice_server_has_gstreamer
  tests += [['test-gst', false]]
  if get_option('extra-checks')
    test('test-video-encoders', files('test-video-encoders'))
  endif
endif

foreach t : tests
  test_name = t[0]
  is_test = t[1]
  exe = executable(test_name,
                   sources : '@0@.c'.format(test_name),
                   link_with : test_libs,
                   include_directories : test_lib_include,
                   c_args : ['-DSPICE_TOP_SRCDIR="@0@"'.format(meson.source_root())],
                   dependencies : test_lib_deps,
                   install : false)

  if is_test
    test(test_name, exe)
  endif
endforeach

executable('spice-server-replay',
           sources : ['replay.c', join_paths('..', 'event-loop.c'), 'basic-event-loop.c', 'basic-event-loop.h'],
           link_with : spice_server_shared_lib,
           include_directories : test_lib_include,
           dependencies : test_lib_deps,
           install : false)

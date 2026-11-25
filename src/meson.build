subdir('common')

exe_mavlink_router = executable('mavlink-routerd',
        [
         'autolog.cpp',
         'binlog.cpp',
         'dedup.cpp',
         'endpoint.cpp',
         'git_version.cpp',
         'logendpoint.cpp',
         'main.cpp',
         'mainloop.cpp',
         'pollable.cpp',
         'api/api_methods.cpp',
         'api/api_server.cpp',
         'timeout.cpp',
         'ulog.cpp',
         'tlog.cpp',
         version_h,
        ],
        include_directories: [src_inc, mavlink_inc],
        dependencies: [
                dep_math,
                dep_rt,
                dep_thread,
                dep_json,
        ],
        link_with: libcommon_private,
        install: true,
)

if dep_gtest.found()
  exe_mainloop_test = executable('mainloop_test',
          [
            'autolog.cpp',
            'binlog.cpp',
            'dedup.cpp',
            'endpoint.cpp',
            'logendpoint.cpp',
            'mainloop.cpp',
            'mainloop_test.cpp',
            'pollable.cpp',
            'api/api_methods.cpp',
            'api/api_server.cpp',
            'timeout.cpp',
            'ulog.cpp',
            'tlog.cpp',
            version_h,
          ],
          include_directories: [src_inc, mavlink_inc],
          dependencies : [
            dep_gtest,
            dep_rt,
            dep_json,
          ],
          link_with: libcommon_private,
          install: false,
          cpp_args: ['-w']
  )
  test('mainloop', exe_mainloop_test)

  exe_endpoints_test = executable('endpoints_test',
          [
            'autolog.cpp',
            'binlog.cpp',
            'dedup.cpp',
            'endpoint.cpp',
            'endpoints_test.cpp',
            'logendpoint.cpp',
            'mainloop.cpp',
            'pollable.cpp',
            'api/api_methods.cpp',
            'api/api_server.cpp',
            'timeout.cpp',
            'ulog.cpp',
            'tlog.cpp',
            version_h,
          ],
          include_directories: [src_inc, mavlink_inc],
          dependencies : [
            dep_gtest,
            dep_rt,
            dep_json,
          ],
          link_with: libcommon_private,
          install: false,
          cpp_args: ['-w']
  )
  test('endpoints', exe_endpoints_test)

  exe_api_test = executable('api_test',
          [
            'autolog.cpp',
            'binlog.cpp',
            'dedup.cpp',
            'endpoint.cpp',
            'api/api_test.cpp',
            'logendpoint.cpp',
            'mainloop.cpp',
            'pollable.cpp',
            'api/api_methods.cpp',
            'api/api_server.cpp',
            'timeout.cpp',
            'ulog.cpp',
            'tlog.cpp',
            version_h,
          ],
          include_directories: [src_inc, mavlink_inc],
          dependencies : [
            dep_gtest,
            dep_rt,
            dep_json,
          ],
          link_with: libcommon_private,
          install: false,
          cpp_args: ['-w']
  )
  test('api', exe_api_test)
endif

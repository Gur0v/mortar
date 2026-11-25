project('mortar', 'cpp', version: '0.1.0', default_options: ['cpp_std=c++23', 'warning_level=3'])

xcb = dependency('xcb')
mortar = executable('mortar', 'mortar/main.cpp', dependencies: xcb, install: true)

client = executable('test-client', 'tests/client.cpp', dependencies: xcb)
test('x11', find_program('ruby'), args: [files('tests/x11.rb'), mortar, client], timeout: 30)

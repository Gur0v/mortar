project('mortar', 'cpp', version: '0.1.0', default_options: ['cpp_std=c++23', 'warning_level=3'])

xcb = dependency('xcb')
mortar = executable('mortar', 'mortar/main.cpp', dependencies: xcb, install: true)

test('x11', find_program('ruby'), args: [files('tests/x11.rb'), mortar], timeout: 30)

project('mortar', 'cpp', version: '0.1.0', default_options: ['cpp_std=c++23', 'warning_level=3'])

xcb = dependency('xcb')
xcb_randr = dependency('xcb-randr')
mortar = executable('mortar', 'mortar/main.cpp', 'mortar/layout.cpp', dependencies: [xcb, xcb_randr], install: true)

if get_option('tests')
  layout_test = executable('test-layout', 'tests/layout.cpp', 'mortar/layout.cpp')
  test('layout', layout_test)
  client = executable('test-client', 'tests/client.cpp', dependencies: xcb)
  test('x11', find_program('ruby'), args: [files('tests/x11.rb'), mortar, client], timeout: 30)
endif

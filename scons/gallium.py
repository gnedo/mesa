"""gallium

Frontend-tool for Gallium3D architecture.

"""

#
# Copyright 2008 VMware, Inc.
# All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sub license, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice (including the
# next paragraph) shall be included in all copies or substantial portions
# of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
# IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
# ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#

from __future__ import print_function

import distutils.version
import os
import os.path
import re
import subprocess
import platform as host_platform
import sys
import tempfile

import SCons.Action
import SCons.Builder
import SCons.Scanner


def symlink(target, source, env):
    target = str(target[0])
    source = str(source[0])
    if os.path.islink(target) or os.path.exists(target):
        os.remove(target)
    os.symlink(os.path.basename(source), target)

def install(env, source, subdir):
    target_dir = os.path.join(env.Dir('#.').srcnode().abspath, env['build_dir'], subdir)
    return env.Install(target_dir, source)

def install_program(env, source):
    return install(env, source, 'bin')

def install_shared_library(env, sources, version = ()):
    targets = []
    install_dir = os.path.join(env.Dir('#.').srcnode().abspath, env['build_dir'])
    version = tuple(map(str, version))
    if env['SHLIBSUFFIX'] == '.dll':
        dlls = env.FindIxes(sources, 'SHLIBPREFIX', 'SHLIBSUFFIX')
        targets += install(env, dlls, 'bin')
        libs = env.FindIxes(sources, 'LIBPREFIX', 'LIBSUFFIX')
        targets += install(env, libs, 'lib')
    else:
        for source in sources:
            target_dir =  os.path.join(install_dir, 'lib')
            target_name = '.'.join((str(source),) + version)
            last = env.InstallAs(os.path.join(target_dir, target_name), source)
            targets += last
            while len(version):
                version = version[:-1]
                target_name = '.'.join((str(source),) + version)
                action = SCons.Action.Action(symlink, "  Symlinking $TARGET ...")
                last = env.Command(os.path.join(target_dir, target_name), last, action) 
                targets += last
    return targets


def msvc2013_compat(env):
    if env['gcc']:
        env.Append(CCFLAGS = [
            '-Werror=vla',
            '-Werror=pointer-arith',
        ])


def unit_test(env, test_name, program_target, args=None):
    env.InstallProgram(program_target)

    cmd = [program_target[0].abspath]
    if args is not None:
        cmd += args
    cmd = ' '.join(cmd)

    # http://www.scons.org/wiki/UnitTests
    action = SCons.Action.Action(cmd, "  Running $SOURCE ...")
    alias = env.Alias(test_name, program_target, action)
    env.AlwaysBuild(alias)
    env.Depends('check', alias)


def num_jobs():
    try:
        return int(os.environ['NUMBER_OF_PROCESSORS'])
    except (ValueError, KeyError):
        pass

    try:
        return os.sysconf('SC_NPROCESSORS_ONLN')
    except (ValueError, OSError, AttributeError):
        pass

    try:
        return int(os.popen2("sysctl -n hw.ncpu")[1].read())
    except ValueError:
        pass

    return 1


def check_cc(env, cc, expr, cpp_opt = '-E'):
    # Invoke C-preprocessor to determine whether the specified expression is
    # true or not.

    sys.stdout.write('Checking for %s ... ' % cc)

    source = tempfile.NamedTemporaryFile(suffix='.c', delete=False)
    source.write('#if !(%s)\n#error\n#endif\n' % expr)
    source.close()

    # sys.stderr.write('%r %s %s\n' % (env['CC'], cpp_opt, source.name));

    pipe = SCons.Action._subproc(env, env.Split(env['CC']) + [cpp_opt, source.name],
                                 stdin = 'devnull',
                                 stderr = 'devnull',
                                 stdout = 'devnull')
    result = pipe.wait() == 0

    os.unlink(source.name)

    sys.stdout.write(' %s\n' % ['no', 'yes'][int(bool(result))])
    return result

def check_header(env, header):
    '''Check if the header exist'''

    conf = SCons.Script.Configure(env)
    have_header = False

    if conf.CheckHeader(header):
        have_header = True

    env = conf.Finish()
    return have_header

def check_functions(env, functions):
    '''Check if all of the functions exist'''

    conf = SCons.Script.Configure(env)
    have_functions = True

    for function in functions:
        if not conf.CheckFunc(function):
            have_functions = False

    env = conf.Finish()
    return have_functions

def check_prog(env, prog):
    """Check whether this program exists."""

    sys.stdout.write('Checking for %s ... ' % prog)

    result = env.Detect(prog)

    sys.stdout.write(' %s\n' % ['no', 'yes'][int(bool(result))])
    return result


def generate(env):
    """Common environment generation code"""

    # Tell tools which machine to compile for
    env['TARGET_ARCH'] = env['machine']
    env['MSVS_ARCH'] = env['machine']

    # Toolchain
    platform = env['platform']
    env.Tool(env['toolchain'])

    # Allow override compiler and specify additional flags from environment
    if 'CC' in os.environ:
        env['CC'] = os.environ['CC']
    if 'CFLAGS' in os.environ:
        env['CCFLAGS'] += SCons.Util.CLVar(os.environ['CFLAGS'])
    if 'CXX' in os.environ:
        env['CXX'] = os.environ['CXX']
    if 'CXXFLAGS' in os.environ:
        env['CXXFLAGS'] += SCons.Util.CLVar(os.environ['CXXFLAGS'])
    if 'LDFLAGS' in os.environ:
        env['LINKFLAGS'] += SCons.Util.CLVar(os.environ['LDFLAGS'])

    # Detect gcc/clang not by executable name, but through pre-defined macros
    # as autoconf does, to avoid drawing wrong conclusions when using tools
    # that overrice CC/CXX like scan-build.
    env['gcc_compat'] = 0
    env['clang'] = 0
    env['msvc'] = 0
    if host_platform.system() == 'Windows':
        env['msvc'] = check_cc(env, 'MSVC', 'defined(_MSC_VER)', '/E')
    if not env['msvc']:
        env['gcc_compat'] = check_cc(env, 'GCC', 'defined(__GNUC__)')
    env['clang'] = check_cc(env, 'Clang', '__clang__')
    env['gcc'] = env['gcc_compat'] and not env['clang']
    env['suncc'] = env['platform'] == 'sunos' and os.path.basename(env['CC']) == 'cc'
    env['icc'] = 'icc' == os.path.basename(env['CC'])

    # shortcuts
    machine = env['machine']
    platform = env['platform']
    x86 = env['machine'] == 'x86'
    ppc = env['machine'] == 'ppc'
    gcc_compat = env['gcc_compat']
    msvc = env['msvc']
    suncc = env['suncc']
    icc = env['icc']

    # Determine whether we are cross compiling; in particular, whether we need
    # to compile code generators with a different compiler as the target code.
    hosthost_platform = host_platform.system().lower()
    if hosthost_platform.startswith('cygwin'):
        hosthost_platform = 'cygwin'
    host_machine = os.environ.get('PROCESSOR_ARCHITEW6432', os.environ.get('PROCESSOR_ARCHITECTURE', host_platform.machine()))
    host_machine = {
        'x86': 'x86',
        'i386': 'x86',
        'i486': 'x86',
        'i586': 'x86',
        'i686': 'x86',
        'ppc' : 'ppc',
        'AMD64': 'x86_64',
        'x86_64': 'x86_64',
    }.get(host_machine, 'generic')
    env['crosscompile'] = platform != hosthost_platform
    if machine == 'x86_64' and host_machine != 'x86_64':
        env['crosscompile'] = True
    env['hostonly'] = False

    # Backwards compatability with the debug= profile= options
    if env['build'] == 'debug':
        if not env['debug']:
            print('scons: warning: debug option is deprecated and will be removed eventually; use instead')
            print('')
            print(' scons build=release')
            print('')
            env['build'] = 'release'
        if env['profile']:
            print('scons: warning: profile option is deprecated and will be removed eventually; use instead')
            print('')
            print(' scons build=profile')
            print('')
            env['build'] = 'profile'
    if False:
        # Enforce SConscripts to use the new build variable
        env.popitem('debug')
        env.popitem('profile')
    else:
        # Backwards portability with older sconscripts
        if env['build'] in ('debug', 'checked'):
            env['debug'] = True
            env['profile'] = False
        if env['build'] == 'profile':
            env['debug'] = False
            env['profile'] = True
        if env['build'] == 'release':
            env['debug'] = False
            env['profile'] = False

    # Put build output in a separate dir, which depends on the current
    # configuration. See also http://www.scons.org/wiki/AdvancedBuildExample
    build_topdir = 'build'
    build_subdir = env['platform']
    if env['embedded']:
        build_subdir =  'embedded-' + build_subdir
    if env['machine'] != 'generic':
        build_subdir += '-' + env['machine']
    if env['build'] != 'release':
        build_subdir += '-' +  env['build']
    build_dir = os.path.join(build_topdir, build_subdir)
    # Place the .sconsign file in the build dir too, to avoid issues with
    # different scons versions building the same source file
    env['build_dir'] = build_dir
    env.SConsignFile(os.path.join(build_dir, '.sconsign'))
    if 'SCONS_CACHE_DIR' in os.environ:
        print('scons: Using build cache in %s.' % (os.environ['SCONS_CACHE_DIR'],))
        env.CacheDir(os.environ['SCONS_CACHE_DIR'])
    env['CONFIGUREDIR'] = os.path.join(build_dir, 'conf')
    env['CONFIGURELOG'] = os.path.join(os.path.abspath(build_dir), 'config.log')

    # Parallel build
    if env.GetOption('num_jobs') <= 1:
        env.SetOption('num_jobs', num_jobs())

    # Speed up dependency checking.  See
    # - https://github.com/SCons/scons/wiki/GoFastButton
    # - https://bugs.freedesktop.org/show_bug.cgi?id=109443

    # Scons version string has consistently been in this format:
    # MajorVersion.MinorVersion.Patch[.alpha/beta.yyyymmdd]
    # so this formula should cover all versions regardless of type
    # stable, alpha or beta.
    # For simplicity alpha and beta flags are removed.

    scons_version = distutils.version.StrictVersion('.'.join(SCons.__version__.split('.')[:3]))
    if scons_version < distutils.version.StrictVersion('3.0.2') or \
       scons_version > distutils.version.StrictVersion('3.0.4'):
        env.Decider('MD5-timestamp')
    env.SetOption('max_drift', 60)

    # C preprocessor options
    cppdefines = []
    cppdefines += [
        '__STDC_CONSTANT_MACROS',
        '__STDC_FORMAT_MACROS',
        '__STDC_LIMIT_MACROS',
        'HAVE_SCONS',
    ]
    if env['build'] in ('debug', 'checked'):
        cppdefines += ['DEBUG']
    else:
        cppdefines += ['NDEBUG']
    if env['build'] == 'profile':
        cppdefines += ['PROFILE']
    if env['platform'] in ('posix', 'linux', 'freebsd', 'darwin'):
        cppdefines += [
            '_POSIX_SOURCE',
            ('_POSIX_C_SOURCE', '199309L'),
            '_SVID_SOURCE',
            '_BSD_SOURCE',
            '_GNU_SOURCE',
            '_DEFAULT_SOURCE',
        ]
        if env['platform'] == 'darwin':
            cppdefines += [
                '_DARWIN_C_SOURCE',
                'GLX_USE_APPLEGL',
                'GLX_DIRECT_RENDERING',
                'BUILDING_MESA',
            ]
        else:
            cppdefines += [
                'GLX_DIRECT_RENDERING',
                'GLX_INDIRECT_RENDERING',
            ]

        if check_header(env, 'xlocale.h'):
            cppdefines += ['HAVE_XLOCALE_H']

        if check_header(env, 'endian.h'):
            cppdefines += ['HAVE_ENDIAN_H']

        if check_functions(env, ['strtod_l', 'strtof_l']):
            cppdefines += ['HAVE_STRTOD_L']

        if check_functions(env, ['random_r']):
            cppdefines += ['HAVE_RANDOM_R']

        if check_functions(env, ['timespec_get']):
            cppdefines += ['HAVE_TIMESPEC_GET']

        if check_header(env, 'sys/shm.h'):
            cppdefines += ['HAVE_SYS_SHM_H']

    if platform == 'windows':
        cppdefines += [
            'WIN32',
            '_WINDOWS',
            #'_UNICODE',
            #'UNICODE',
            # http://msdn.microsoft.com/en-us/library/aa383745.aspx
            ('_WIN32_WINNT', '0x0601'),
            ('WINVER', '0x0601'),
        ]
        if gcc_compat:
            cppdefines += [('__MSVCRT_VERSION__', '0x0700')]
        if msvc:
            cppdefines += [
                'VC_EXTRALEAN',
                '_USE_MATH_DEFINES',
                '_CRT_SECURE_NO_WARNINGS',
                '_CRT_SECURE_NO_DEPRECATE',
                '_SCL_SECURE_NO_WARNINGS',
                '_SCL_SECURE_NO_DEPRECATE',
                '_ALLOW_KEYWORD_MACROS',
                '_HAS_EXCEPTIONS=0', # Tell C++ STL to not use exceptions
            ]
        if env['build'] in ('debug', 'checked'):
            cppdefines += ['_DEBUG']
    if env['embedded']:
        cppdefines += ['EMBEDDED_DEVICE']
    env.Append(CPPDEFINES = cppdefines)

    # C compiler options
    cflags = [] # C
    cxxflags = [] # C++
    ccflags = [] # C & C++
    if gcc_compat:
        if env['build'] == 'debug':
            ccflags += ['-O0']
        else:
            ccflags += ['-O3']
        if env['gcc']:
            # gcc's builtin memcmp is slower than glibc's
            # http://gcc.gnu.org/bugzilla/show_bug.cgi?id=43052
            ccflags += ['-fno-builtin-memcmp']
        # Work around aliasing bugs - developers should comment this out
        ccflags += ['-fno-strict-aliasing']
        ccflags += ['-g']
        if env['build'] in ('checked', 'profile') or env['asan']:
            # See http://code.google.com/p/jrfonseca/wiki/Gprof2Dot#Which_options_should_I_pass_to_gcc_when_compiling_for_profiling?
            ccflags += [
                '-fno-omit-frame-pointer',
            ]
            if env['gcc']:
                ccflags += ['-fno-optimize-sibling-calls']
        if env['machine'] == 'x86':
            ccflags += [
                '-m32',
                #'-march=pentium4',
            ]
            if platform != 'haiku':
                # NOTE: We need to ensure stack is realigned given that we
                # produce shared objects, and have no control over the stack
                # alignment policy of the application. Therefore we need
                # -mstackrealign ore -mincoming-stack-boundary=2.
                #
                # XXX: We could have SSE without -mstackrealign if we always used
                # __attribute__((force_align_arg_pointer)), but that's not
                # always the case.
                ccflags += [
                    '-mstackrealign', # ensure stack is aligned
                    '-msse', '-msse2', # enable SIMD intrinsics
                    '-mfpmath=sse', # generate SSE floating-point arithmetic
                ]
            if platform in ['windows', 'darwin']:
                # Workaround http://gcc.gnu.org/bugzilla/show_bug.cgi?id=37216
                ccflags += ['-fno-common']
            if platform in ['haiku']:
                # Make optimizations compatible with Pentium or higher on Haiku
                ccflags += [
                    '-mstackrealign', # ensure stack is aligned
                    '-march=i586', # Haiku target is Pentium
                    '-mtune=i686' # use i686 where we can
                ]
        if env['machine'] == 'x86_64':
            ccflags += ['-m64']
            if platform == 'darwin':
                ccflags += ['-fno-common']
        if env['platform'] not in ('cygwin', 'haiku', 'windows'):
            ccflags += ['-fvisibility=hidden']
        # See also:
        # - http://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html
        ccflags += [
            '-Wall',
            '-Wno-long-long',
            '-fmessage-length=0', # be nice to Eclipse
        ]
        cflags += [
            '-Wmissing-prototypes',
            '-std=gnu99',
        ]
    if icc:
        cflags += [
            '-std=gnu99',
        ]
    if msvc:
        # See also:
        # - http://msdn.microsoft.com/en-us/library/19z1t1wy.aspx
        # - cl /?
        if env['build'] == 'debug':
            ccflags += [
              '/Od', # disable optimizations
              '/Oi', # enable intrinsic functions
            ]
        else:
            ccflags += [
                '/O2', # optimize for speed
            ]
        if env['build'] == 'release':
            if not env['clang']:
                ccflags += [
                    '/GL', # enable whole program optimization
                ]
        else:
            ccflags += [
                '/Oy-', # disable frame pointer omission
            ]
        ccflags += [
            '/W3', # warning level
            '/wd4018', # signed/unsigned mismatch
            '/wd4056', # overflow in floating-point constant arithmetic
            '/wd4244', # conversion from 'type1' to 'type2', possible loss of data
            '/wd4267', # 'var' : conversion from 'size_t' to 'type', possible loss of data
            '/wd4305', # truncation from 'type1' to 'type2'
            '/wd4351', # new behavior: elements of array 'array' will be default initialized
            '/wd4756', # overflow in constant arithmetic
            '/wd4800', # forcing value to bool 'true' or 'false' (performance warning)
            '/wd4996', # disable deprecated POSIX name warnings
        ]
        if env['clang']:
            ccflags += [
                '-Wno-microsoft-enum-value', # enumerator value is not representable in underlying type 'int'
            ]
        if env['machine'] == 'x86':
            ccflags += [
                '/arch:SSE2', # use the SSE2 instructions (default since MSVC 2012)
            ]
        if platform == 'windows':
            ccflags += [
                # TODO
            ]
        # Automatic pdb generation
        # See http://scons.tigris.org/issues/show_bug.cgi?id=1656
        env.EnsureSConsVersion(0, 98, 0)
        env['PDB'] = '${TARGET.base}.pdb'
    env.Append(CCFLAGS = ccflags)
    env.Append(CFLAGS = cflags)
    env.Append(CXXFLAGS = cxxflags)

    if env['platform'] == 'windows' and msvc:
        # Choose the appropriate MSVC CRT
        # http://msdn.microsoft.com/en-us/library/2kzt1wy3.aspx
        if env['build'] in ('debug', 'checked'):
            env.Append(CCFLAGS = ['/MTd'])
            env.Append(SHCCFLAGS = ['/LDd'])
        else:
            env.Append(CCFLAGS = ['/MT'])
            env.Append(SHCCFLAGS = ['/LD'])
    
    # Static code analysis
    if env['analyze']:
        if env['msvc']:
            # http://msdn.microsoft.com/en-us/library/ms173498.aspx
            env.Append(CCFLAGS = [
                '/analyze',
                #'/analyze:log', '${TARGET.base}.xml',
                '/wd28251', # Inconsistent annotation for function
            ])
        if env['clang']:
            # scan-build will produce more comprehensive output
            env.Append(CCFLAGS = ['--analyze'])

    # https://github.com/google/sanitizers/wiki/AddressSanitizer
    if env['asan']:
        if gcc_compat:
            env.Append(CCFLAGS = [
                '-fsanitize=address',
            ])
            env.Append(LINKFLAGS = [
                '-fsanitize=address',
            ])

    # Assembler options
    if gcc_compat:
        if env['machine'] == 'x86':
            env.Append(ASFLAGS = ['-m32'])
        if env['machine'] == 'x86_64':
            env.Append(ASFLAGS = ['-m64'])

    # Linker options
    linkflags = []
    shlinkflags = []
    if gcc_compat:
        if env['machine'] == 'x86':
            linkflags += ['-m32']
        if env['machine'] == 'x86_64':
            linkflags += ['-m64']
        if env['platform'] not in ('darwin'):
            shlinkflags += [
                '-Wl,-Bsymbolic',
            ]
        # Handle circular dependencies in the libraries
        if env['platform'] in ('darwin'):
            pass
        else:
            env['_LIBFLAGS'] = '-Wl,--start-group ' + env['_LIBFLAGS'] + ' -Wl,--end-group'
        if env['platform'] == 'windows':
            linkflags += [
                '-Wl,--nxcompat', # DEP
                '-Wl,--dynamicbase', # ASLR
            ]
            # Avoid depending on gcc runtime DLLs
            linkflags += ['-static-libgcc']
            if 'w64' in env['CC'].split('-'):
                linkflags += ['-static-libstdc++']
            # Handle the @xx symbol munging of DLL exports
            shlinkflags += ['-Wl,--enable-stdcall-fixup']
            #shlinkflags += ['-Wl,--kill-at']
    if msvc:
        if env['build'] == 'release' and not env['clang']:
            # enable Link-time Code Generation
            linkflags += ['/LTCG']
            env.Append(ARFLAGS = ['/LTCG'])
    if platform == 'windows' and msvc:
        # See also:
        # - http://msdn2.microsoft.com/en-us/library/y0zzbyt4.aspx
        linkflags += [
            '/fixed:no',
            '/incremental:no',
            '/dynamicbase', # ASLR
            '/nxcompat', # DEP
        ]
    env.Append(LINKFLAGS = linkflags)
    env.Append(SHLINKFLAGS = shlinkflags)

    # We have C++ in several libraries, so always link with the C++ compiler
    if gcc_compat:
        env['LINK'] = env['CXX']

    # Default libs
    libs = []
    if env['platform'] in ('darwin', 'freebsd', 'linux', 'posix', 'sunos'):
        libs += ['m', 'pthread', 'dl']
    if env['platform'] in ('linux',):
        libs += ['rt']
    if env['platform'] in ('haiku'):
        libs += ['root', 'be', 'network', 'translation']
    env.Append(LIBS = libs)

    # OpenMP
    if env['openmp']:
        if env['msvc']:
            env.Append(CCFLAGS = ['/openmp'])
            # When building openmp release VS2008 link.exe crashes with LNK1103 error.
            # Workaround: overwrite PDB flags with empty value as it isn't required anyways
            if env['build'] == 'release':
                env['PDB'] = ''
        if env['gcc']:
            env.Append(CCFLAGS = ['-fopenmp'])
            env.Append(LIBS = ['gomp'])

    # Load tools
    env.Tool('lex')
    if env['msvc']:
        env.Append(LEXFLAGS = [
            # Force flex to use const keyword in prototypes, as relies on
            # __cplusplus or __STDC__ macro to determine whether it's safe to
            # use const keyword, but MSVC never defines __STDC__ unless we
            # disable all MSVC extensions.
            '-DYY_USE_CONST=',
        ])
        # Flex relies on __STDC_VERSION__>=199901L to decide when to include
        # C99 inttypes.h.  We always have inttypes.h available with MSVC
        # (either the one bundled with MSVC 2013, or the one we bundle
        # ourselves), but we can't just define __STDC_VERSION__ without
        # breaking stuff, as MSVC doesn't fully support C99.  There's also no
        # way to premptively include stdint.
        env.Append(CCFLAGS = ['-FIinttypes.h'])
    if host_platform.system() == 'Windows':
        # Prefer winflexbison binaries, as not only they are easier to install
        # (no additional dependencies), but also better Windows support.
        if check_prog(env, 'win_flex'):
            env["LEX"] = 'win_flex'
            env.Append(LEXFLAGS = [
                # windows compatibility (uses <io.h> instead of <unistd.h> and
                # _isatty, _fileno functions)
                '--wincompat'
            ])

    env.Tool('yacc')
    if host_platform.system() == 'Windows':
        if check_prog(env, 'win_bison'):
            env["YACC"] = 'win_bison'

    if env['llvm']:
        env.Tool('llvm')
    
    # Custom builders and methods
    env.Tool('custom')
    env.AddMethod(install_program, 'InstallProgram')
    env.AddMethod(install_shared_library, 'InstallSharedLibrary')
    env.AddMethod(msvc2013_compat, 'MSVC2013Compat')
    env.AddMethod(unit_test, 'UnitTest')

    env.PkgCheckModules('X11', ['x11', 'xext', 'xdamage >= 1.1', 'xfixes', 'glproto >= 1.4.13', 'dri2proto >= 2.8'])
    env.PkgCheckModules('XCB', ['x11-xcb', 'xcb-glx >= 1.8.1', 'xcb-dri2 >= 1.8'])
    env.PkgCheckModules('XF86VIDMODE', ['xxf86vm'])
    env.PkgCheckModules('DRM', ['libdrm >= 2.4.75'])

    if not os.path.exists("src/util/format_srgb.c"):
        print("Checking for Python Mako module (>= 0.8.0)... ", end='')
        try:
            import mako
        except ImportError:
            print("no")
            exit(1)
        if distutils.version.StrictVersion(mako.__version__) < distutils.version.StrictVersion('0.8.0'):
            print("no")
            exit(1)
        print("yes")

    if env['x11']:
        env.Append(CPPPATH = env['X11_CPPPATH'])

    env['dri'] = env['x11'] and env['drm']

    # for debugging
    #print env.Dump()


def exists(env):
    return 1

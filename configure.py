# vim: set sts=2 ts=8 sw=2 tw=99 et:
import sys
try:
  from ambuild2 import run, util
except:
  sys.stderr.write('AMBuild 2.2+ must be installed to build this project.\n')
  sys.stderr.write('http://www.alliedmods.net/ambuild\n')
  sys.exit(1)

parser = run.BuildParser(sourcePath=sys.path[0], api='2.2')
parser.options.add_argument('-n', '--plugin-name', type=str, dest='plugin_name', default=None,
                       help='Plugin name')
parser.options.add_argument('--hl2sdk-root', type=str, dest='hl2sdk_root', default=None,
                       help='Root search folder for HL2SDKs')
parser.options.add_argument('--hl2sdk-manifests', type=str, dest='hl2sdk_manifests', default=None,
                       help='HL2SDK manifests source tree folder')
parser.options.add_argument('--mms_path', type=str, dest='mms_path', default=None,
                       help='Metamod:Source source tree folder')
parser.options.add_argument('--mariadb-prefix', type=str, dest='mariadb_prefix', default='/opt/mariadb',
                       help='mariadb-connector-c install prefix (static)')
parser.options.add_argument('--enable-debug', action='store_const', const='1', dest='debug',
                       help='Enable debugging symbols')
parser.options.add_argument('--enable-optimize', action='store_const', const='1', dest='opt',
                       help='Enable optimization')
parser.options.add_argument('-s', '--sdks', default='cs2', dest='sdks',
                       help='Build against specified SDKs (default: cs2)')
parser.options.add_argument('--targets', type=str, dest='targets', default='x86_64',
                            help='Target architectures (default: x86_64)')
parser.Configure()

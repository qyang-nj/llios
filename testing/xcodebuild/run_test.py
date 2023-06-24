#!/usr/bin/env python3

"""A script to demonstrate how to run tests with `xcodebuild test-without-building`"""

import argparse
import tempfile
import shutil
import subprocess
import plistlib
from pathlib import Path, PurePath

parser = argparse.ArgumentParser(description='Run tests with `xcodebuild test-without-building`')
parser.add_argument('--test-bundle', required=True, help='The path of test bundle (.xctest)')
parser.add_argument('--host-app', help='The path of host app (.app)')
parser.add_argument('--filter', action='append', help='Select tests to run. format: Test-Class-Name[/Test-Method-Name]')
parser.add_argument('--skip', action='append', help='Select tests not to run. format: Test-Class-Name[/Test-Method-Name]')
parser.add_argument('--show-xctestrun', action='store_true', help='Show the content of xctestrun file')
args = parser.parse_args()

test_bundle_path = Path(args.test_bundle)
if not test_bundle_path.exists():
    raise Exception("Test bundle not found")

test_bundle_name = test_bundle_path.name

test_root = tempfile.TemporaryDirectory()
shutil.copytree(test_bundle_path, PurePath(test_root.name, test_bundle_name))

test_host_path = "__PLATFORMS__/iPhoneSimulator.platform/Developer/Library/Xcode/Agents/xctest"

if args.host_app is not None:
    host_app_path = Path(args.host_app)
    if not test_bundle_path.exists():
        raise Exception("Host app not found")

    host_app_name = host_app_path.name
    shutil.copytree(host_app_path, PurePath(test_root.name, host_app_name))

    test_host_path = f"__TESTROOT__/{host_app_name}"

xctestrun_plist = dict(
    Runner = dict(
        TestBundlePath = f"__TESTROOT__/{test_bundle_name}",
        TestHostPath = test_host_path,
        TestingEnvironmentVariables = dict(
            DYLD_FRAMEWORK_PATH = "__TESTROOT__:__PLATFORMS__/iPhoneSimulator.platform/Developer/Library/Frameworks:__PLATFORMS__/iPhoneSimulator.platform/Developer/Library/PrivateFrameworks",
            DYLD_INSERT_LIBRARIES = "__PLATFORMS__/iPhoneSimulator.platform/Developer/usr/lib/libXCTestBundleInject.dylib",
            DYLD_LIBRARY_PATH = "__TESTROOT__:__PLATFORMS__/iPhoneSimulator.platform/Developer/usr/lib:",
        )
    )
)

if args.filter is not None:
    xctestrun_plist['Runner']['OnlyTestIdentifiers'] = args.filter

if args.skip is not None:
    xctestrun_plist['Runner']['SkipTestIdentifiers'] = args.skip

xctestrun_content = plistlib.dumps(xctestrun_plist).decode()

if args.show_xctestrun:
    print(xctestrun_content)

xctestrun_path = PurePath(test_root.name, 'xctestrun.plist')
with open(xctestrun_path, 'w') as f:
    f.write(xctestrun_content)

subprocess.run(["xcodebuild", "test-without-building", "-xctestrun", xctestrun_path, "-destination", "platform=iOS Simulator,name=iPhone 14 Pro,OS=latest"])

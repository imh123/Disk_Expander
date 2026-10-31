# Third-party notices

Disk Expander is built with Qt 6.6.3 and dynamically links the following Qt
modules:

- Qt Core
- Qt Gui
- Qt Widgets
- Qt Svg (only for the SVG icon engine plugin)

Qt is available under the GNU Lesser General Public License version 3
(LGPLv3). The binary distribution in this repository keeps Qt as separate DLL
files so that users can replace them with their own compatible builds, as
required by the LGPL.

- Qt licensing overview: https://www.qt.io/licensing/open-source-lgpl-obligations
- Qt source code: https://download.qt.io/official_releases/qt/6.6/6.6.3/
- LGPLv3 / GPLv3 license text: see `LICENSES/Qt-LICENSE.txt`
- Qt copyright notices: see `LICENSES/Qt-LICENSE.txt`

The application also redistributes the Microsoft Visual C++ runtime
(`msvcp140.dll`, `msvcp140_1.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`)
as required by the MSVC toolchain. These files are covered by the Microsoft
Visual Studio license terms.

Windows is a trademark of Microsoft Corporation. Qt is a trademark of The Qt
Company.

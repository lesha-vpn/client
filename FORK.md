# Лёша VPN fork of AmneziaVPN

This repo (`lesha-vpn/client`, default branch `dev`) is a fork of
[amnezia-vpn/amnezia-client](https://github.com/amnezia-vpn/amnezia-client). It ships as
**Лёша VPN** (`LeshaVPN`, app id `ru.kiraind.vpn`) and installs side by side with the official
AmneziaVPN. Its purpose: log in to a self-hosted VPN coordinator (Telegram-backed account) and get
auto-updated AmneziaWG configs, with Amnezia's own commercial/gateway features removed.

Upstream base of the fork: `f1af8ef`. Inventory: `git log --oneline f1af8ef..HEAD`,
`git diff --stat f1af8ef..HEAD` (after each upstream sync, the base becomes the merged upstream
tag; use `git log --oneline --no-merges upstream/<tag>..dev` to list fork-only commits).

To integrate a new upstream release follow `.claude/skills/sync-upstream/SKILL.md`. When a merge
conflicts, find the file below and resolve **by intent**: keep upstream's new behaviour, then
re-apply the fork's change described here.

---

## 1. Coordinator client (fork feature)
Commits: `c1dde1e`, `6818032`, `e70a501`.

Fork-only files (never conflict; upstream doesn't have them, but check that APIs they call still exist):
- `client/core/controllers/coordinator/coordinatorApi.{h,cpp}` — HTTP client for the coordinator API
- `client/core/controllers/coordinator/coordinatorController.{h,cpp}` — login, config sync, import of managed configs into the servers repo
- `client/core/controllers/coordinator/deviceName.{h,cpp}` — device name for the default config name and login label (Android device name / manufacturer + model, macOS computer name, Linux pretty hostname, else hostname) and free text → `[a-z0-9-]` with Cyrillic transliteration; configs are renamed with `PATCH /v1/configs/{id}`
- `client/ui/controllers/coordinator/coordinatorUiController.{h,cpp}` — QML-facing controller (`CoordinatorController` in QML)
- `client/ui/qml/Pages2/PageCoordinatorLogin.qml`, `PageCoordinatorConfigs.qml`
- `client/tests/testCoordinatorController.cpp` (mock API test, `CoordinatorControllerTest`)

Fork hunks in upstream files (conflict-prone):
- `client/core/repositories/secureAppSettingsRepository.{h,cpp}` — `coordinatorBaseUrl/Token/User/Configs/ApiAddress` accessors + `clearCoordinatorAccount()` (app backups no longer exist, §9.3).
- `client/vpnConnection.cpp` (`appendSplitTunnelingConfig`, before `splitTunnelType` is stored) — the last known coordinator
  IP (`coordinatorApiAddress`) is excluded from the tunnel ("all sites" becomes "all except" that one address), so a dead
  entry server can't block the update that replaces it. `CoordinatorApi` retries by that IP (SNI/Host keep the name) when DNS fails.
- `client/secureQSettings.cpp` — `"Coordinator/token"` added to `encryptedKeys`; keychain default derived from `APPLICATION_NAME`.
- `client/core/controllers/coreController.{h,cpp}` — members `m_coordinatorApi`, `m_coordinatorController`, `m_coordinatorUiController`; construction and QML context registration.
- `client/core/controllers/coreSignalHandlers.cpp` — `CoordinatorUiController::errorOccurred` -> `PageController::showErrorMessage(QString)`.
- `client/cmake/sources.cmake` — coordinator `.h/.cpp` in HEADERS/SOURCES; `ui/controllers/coordinator/*` in the UI_CONTROLLERS globs.
- `client/ui/utils/pageEnum.h` — `PageCoordinatorLogin`, `PageCoordinatorConfigs`; `client/ui/qml/qml.qrc` — the two pages.
- `client/ui/qml/Pages2/PageSettings.qml` — `coordinatorAccount` entry ("%1 account" with the display name).
- `client/ui/qml/Pages2/PageStart.qml` (third tab, `onEscapePressed`) and `PageSetupWizardStart.qml` ("Let's get started") —
  when logged in, open `PageCoordinatorConfigs` instead of the connection screen. The third tab shows the fork-only
  `client/images/controls/user.svg` (Feather "user", same style as the other tab icons) instead of upstream's `plus.svg`.
- `client/CMakeLists.txt` — `COORDINATOR_API_URL` cache var -> `-DCOORDINATOR_API_URL`; `deploy/build.sh` / `deploy/build.bat` pass it through.
- `client/tests/CMakeLists.txt` — `test_coordinator_controller` target + `add_test`.
- `client/vpnConnection.cpp` — the coordinator IP is excluded only while signed in (`coordinatorToken` not empty).
- `client/secureQSettings.cpp` (Linux only) — the settings file is `0600` and its directory `0700`, since there is no keychain encryption there.
- `client/core/controllers/coreSignalHandlers.cpp` — `translationsUpdated` -> `ServersUiController::updateModel`, so server descriptions
  ("AmneziaWG (version 3.1)") are rebuilt in the UI language after the first translator is installed.
- `client/ui/qml/Pages2/PageSettingsServerData.qml` — for a managed server, "remove" deletes its config on the coordinator
  (`CoordinatorController.deleteConfig`), so it doesn't stay behind as a dead device.
- `client/ui/qml/Pages2/PageSettingsApplication.qml` — "reset all data" calls `CoordinatorController.forgetDevice()` first
  (revokes this app's configs and the token on the server).
- `client/ui/qml/Pages2/PageStart.qml` — `onRemoveServerFinished` always goes to the main screen (the servers list page is
  unreachable in the fork); `onConfigDeleted` goes to the start screen when the last server is gone.

How to resolve: take upstream's version of the file, then re-add the fork hunk(s) above. If upstream
renamed/refactored something the coordinator uses (servers repository, import controller,
PageController, settings repo), adapt the coordinator code to the new API rather than keeping the old
upstream code. `CoordinatorControllerTest` must pass afterwards.

## 2. MTU handling on import
File: `client/core/controllers/selfhosted/importController.cpp`, `ImportController::processAmneziaConfig`.
Fork change: instead of always overwriting `mtu` with the platform default, keep the issuer's MTU if it
is smaller than the default (coordinator configs use 1280 due to double encapsulation).
How to resolve: keep upstream's surrounding logic; the rule "mtu = issuer MTU if 0 < issuerMtu <= default, else default" must survive.

## 3. Connection-screen options removed
File: `client/ui/qml/Pages2/PageSetupWizardConfigSource.qml` (commits `50d8b95`, `0f472e8`, `8419f3b`).
Fork state: `variants` = `coordinatorLogin` only. Removed: "VPN by Amnezia"
(`amneziaVpn`), self-hosted setup (`selfHostVpn`), `backupRestore`, `restorePurchases`, `siteLink`,
the "Site Amnezia" footer link, and (Noah, 2026-10-13) the "Insert key" field with its Continue button,
the "Other connection options" label, and the `fileOpen`/`qrScan` entries (their QtObjects stay defined,
just not listed), because imported configs don't auto-update. The intro text and the `coordinatorLogin` title/description are fork strings (see Translations).
The header's "more" (⋮) button and its drawer (logs toggle/export, support tag) are removed too (Noah, 2026-10-13):
the header is a plain `BaseHeaderType` "Connection".
How to resolve: take upstream's file for new/changed code, then delete the removed variants again and
keep the fork's list. Any NEW upstream option that goes to Amnezia's gateway/shop/site must also be dropped.
Related: `PageSettingsAbout.qml`, `PageSettingsApplication.qml` (update UI removed, §4),
`PageSettings.qml` (see §11).

## 4. App updates removed
The fork ships through its own GitHub Releases, so upstream's update checker (which asks Amnezia's gateway/S3 for the
latest version and downloads installers) is deleted rather than gated (§9.3): `updateController.*`, `updateUiController.*`,
`Pages2/PageUpdate.qml`, `Components/UpdateSupportDrawer.qml`, `client/client_scripts/` (`mac_installer.sh`), `images/update/*`,
the `CLIENT_ENABLE_APP_UPDATES` cmake option and define, the `IsAppUpdatesEnabled` QML property, the About page's
"check for updates" button, the "auto update check" switch in `PageSettingsApplication.qml`, and the
`APP_IOS_BUNDLE_ID` / `APP_IOS_STORE_URL_FALLBACK` defines (`cmake/branding/ios.cmake`, `core/utils/appUiConfig.h.in`).
How to resolve: never re-add an upstream update path. If upstream changes the deleted files, keep them deleted; if it adds a
new update/check path (new controller, QML button, gateway "latest version" call), drop it.

## 5. Rebrand (names, ids, IPC, sockets, installers)
Commit `7ddc416`. Single source of truth: `client/cmake/branding/*.cmake`:
- `common.cmake`: `CLIENT_TARGET_NAME`/`CLIENT_APPLICATION_NAME` = `LeshaVPN`, `CLIENT_DISPLAY_NAME` = `Лёша VPN`
  (also emitted as `DISPLAY_NAME` UTF-8 escapes in `version.h`), `CLIENT_APP_DOMAIN` = `ru.kiraind.vpn`,
  `CLIENT_SERVICE_NAME` = `LeshaVPN-service`, `CLIENT_ORGANIZATION_NAME` = `kiraind.ru`,
  `CLIENT_APP_INSTANCE_NAME` = `LeshaVPNInstance`, keychain `${APPLICATION_NAME}-Keychain`.
- `android.cmake`: `CLIENT_ANDROID_PACKAGE` = `ru.kiraind.vpn`. `apple.cmake`: bundle ids from `CLIENT_APP_DOMAIN`,
  group `group.ru.kiraind.vpn`, empty `BUILD_VPN_DEVELOPMENT_TEAM`. `ios.cmake`, `macos.cmake`: same idea.
Code that now derives from these macros instead of hard-coded `AmneziaVPN`/`org.amnezia`:
- `ipc/ipc.h` (`IPC_SERVICE_URL = "local:" APPLICATION_NAME "IpcInterface"`), `client/daemon/daemonlocalserver.cpp`,
  `client/mozilla/localsocketcontroller.cpp` (daemon socket names), `client/main.cpp`, `client/amneziaApplication.cpp`,
  `client/core/utils/migrations.cpp`, `client/ui/utils/{notificationHandler,systemTrayNotificationHandler,qAutoStart}.cpp`,
  `client/platforms/windows/**` (service/tunnel/firewall/split-tunnel names), `client/platforms/macos/**`,
  `client/platforms/ios/{Log,NELogController}.swift`,
  `service/server/{CMakeLists.txt,systemservice.cpp,amneziavpn-service.rc.in}`, `client/platforms/windows/amneziavpn.rc.in`.
- Root `CMakeLists.txt`: includes `client/cmake/clientBrandingDefaults.cmake` early; macOS service group `AMN_SERVICE_GROUP` = lowercased app name.
- `client/CMakeLists.txt` / `client/cmake/macos.cmake`: bundle path `${PROJECT}.app`, `MACOSX_BUNDLE_GUI_IDENTIFIER`.
- Android: `client/android/{AndroidManifest.xml,build.gradle,build.gradle.kts}` (applicationId `ru.kiraind.vpn`; Kotlin
  namespace/package `org.amnezia.vpn` is intentionally unchanged), `res/values*/strings.xml` (app name), `src/org/amnezia/vpn/*.kt`
  (notification channel/tile/auth labels), `protocolApi/.../Protocol.kt`.
- Apple: `client/ios/app/{Info.plist.in,main.entitlements}`, `client/ios/networkextension/*`, `client/macos/app/{Info.plist.in,app.entitlements}`,
  `client/macos/networkextension/*.entitlements` (app group written literally — keep in sync with `apple.cmake`).
- Packaging: `cmake/CPack.cmake` (names from branding; release files are `LeshaVPN_<platform>_<version>.<ext>` with hyphens inside `<platform>`, e.g. `LeshaVPN_windows-x64_5.0.3.1-lesha.6.msi`, `LeshaVPN_android11+-universal_….apk`; the macOS package is plain `macos`, not upstream's `macos_x64`, because it is universal arm64+x86_64, own installer GUIDs/ids, WiX culture ru-RU for the Cyrillic name),
  `cmake/CPackOptions.cmake` (macOS pkg signing identity only when set, so unsigned CI builds work), `cmake/sign_binaries.cmake` (ad-hoc signs the macOS bundle when no identity is set, otherwise Apple Silicon kills the app),
  `client/android/build.gradle.kts` (`onlyOssRelease` gradle property: CI packages only the ossRelease variant),
  `deploy/installer/qif/{componentscript.js,controlscript.js}`, deploy data turned into templates:
  `deploy/data/linux/{app.desktop.in,app.service.in,post_install.sh.in,post_uninstall.sh.in}`,
  `deploy/data/macos/{post_install.sh.in,post_uninstall.sh.in,service.plist.in}`,
  `deploy/data/windows/{post_install.cmd.in,post_uninstall.cmd.in}`, `deploy/installer/**/{close_client_patch,service_install_patch}.xml.in`.
How to resolve: never take upstream's literal names/ids. Take upstream's logic, then replace any literal
`AmneziaVPN`, `AmneziaVPN-service`, `AmneziaVPN.ORG`, `org.amnezia.AmneziaVPN`, `amnvpn`, pipe/socket names, GUIDs
with the branding variable/macro. If upstream changed a deploy file the fork renamed to `.in` (e.g. `deploy/data/linux/AmneziaVPN.desktop`),
git shows modify/delete or rename conflicts: port upstream's change into the `.in` template, keep the `@VAR@` placeholders, delete the upstream literal file.
Translated strings mentioning "AmneziaVPN" in user-visible UI should use `Qt.application.displayName` / `DISPLAY_NAME`.

## 6. ЛЁША monogram logo and "Лёша VPN" wordmarks (binary assets)
Commits `9df8e81`, `440d780` (first flipped-logo version), replaced by the ЛЁША monogram. Files: `client/images/{icon.png,app.ico,app.icns,AmneziaVPN.png,amneziaBigLogo.png,AmneziaVPN_Full_logo.svg}`,
`client/images/controls/amnezia.svg`, `client/images/tray/*.png`, `client/android/res/drawable*/logo.png`,
`client/android/res/drawable/ic_amnezia_round.xml`, `client/android/res/mipmap-*/{icon,icon_round,ic_launcher_foreground,ic_banner}.png`,
`client/ios/app/Media.xcassets/AppIcon.appiconset/*.png`, `client/ios/app/launch.png`,
`client/macos/app/Images.xcassets/AppIcon.appiconset/*.png`, `deploy/data/linux/AmneziaVPN.png`, `metadata/en-US/images/icon.png`.
Fork-only generators: `branding/lesha-logo/` (see its README). The logo is a ligature of Л, Ё, Ш, А in the rough "graffiti" style
of the old mark with flat pastel patches; icons keep their original shape and background (circle/square/squircle), only the glyph changes.
Wordmarks (`AmneziaVPN_Full_logo.svg`, `amneziaBigLogo.png`, `ic_banner`) show the text **"Лёша VPN"** set in
`client/fonts/pt-root-ui_vf.ttf`, next to the monogram; `AmneziaVPN.png` and `drawable*/logo.png` are text only.
How to resolve: binary conflicts — keep ours (`git checkout --ours <file>`). If upstream changed/added an icon that contains the logo,
take upstream's new image and re-run the generators in `branding/lesha-logo/` (add new files to the job table in `make_assets.py`).
Never ship the Amnezia logo or the "AmneziaVPN" wordmark.

## 7. CI workflow
Commit `affb205`. `.github/workflows/deploy.yml` is fully rewritten: builds LeshaVPN for Linux, Windows, macOS, Android
(iOS optional via `vars.ENABLE_IOS`), runs tests on Linux, ships only the `.msi` on Windows (no Qt Installer Framework `.exe`), publishes a GitHub Release on `v*` tags and on every push to `dev` that bumps `LESHA_VERSION` (§8), uses repo var `COORDINATOR_API_URL`
and optional signing secrets, never uploads to Amnezia's infra. Upstream's `tag-deploy.yml` and `tag-upload.yml` are deleted.
Build-time trims: runs on `push` only (no `pull_request`, which built every PR commit twice); Android builds only
`arm64-v8a` + `armeabi-v7a` (no x86 Qt installs) and a universal APK without AAB (`client/android/build.gradle.kts` turns off upstream's per-ABI splits and sets `ndk.abiFilters` to every ABI, since the split copy step kept only one ABI; CI fails the build if the APK lacks `lib/armeabi-v7a` or `lib/arm64-v8a`, or if either lib lacks the baked-in `COORDINATOR_API_URL`; `client/cmake/android.cmake` adds `COORDINATOR_API_URL` to `QT_ANDROID_MULTI_ABI_FORWARD_VARS`, otherwise the armeabi-v7a sub-build gets no URL); all four builds use ccache (Windows builds with Ninja for that: `CMAKE_GENERATOR=Ninja`, and `deploy/build.bat` drops the MSBuild-only `/m` for Ninja; `client/CMakeLists.txt` marks the app `WIN32_EXECUTABLE`, because under Ninja CMake otherwise appends `/subsystem:console` and the app opens with a console window); Qt installs are cached (`cache: true`, needs install-qt-action v4: v3 talks to the retired cache API), Android also caches conan packages and Gradle.
How to resolve: keep ours (`git checkout --ours .github/workflows/deploy.yml`; `git rm` the tag-* files if upstream modified them).
Port only useful upstream changes manually (Qt version bump, new conan/toolchain steps, new required modules/packages).
New upstream workflows: delete them unless they are generic and safe (no Amnezia secrets/servers).

## 8. Versioning
- `AMNEZIAVPN_VERSION` (root `CMakeLists.txt`) = the upstream base version the fork is built on (e.g. `5.0.3.1`); `APP_ANDROID_VERSION_CODE` likewise comes from upstream. `APP_VERSION` (upstream's) is now only used by `Constants::versionString()` (Windows tunnel daemon); the gateway code that sent it is deleted (§9.3).
- `LESHA_VERSION` = fork release counter (positive integer, CACHE, default 1), bumped by 1 for every fork release and **never reset**, not even on an upstream merge. Configure fails if it reaches 100.
- Derived in one block of root `CMakeLists.txt` (the only place they are computed), e.g. for `5.0.3.1` + `1`:
  - `LESHA_FULL_VERSION` `5.0.3.1-lesha.1`: About page, `X-App-Version` (via the `APP_FULL_VERSION` define in `version.h.in`), Android versionName, package file names.
  - `LESHA_NUMERIC_VERSION` `5.0.3001` (MAJOR.MINOR.PATCH*1000+N): MSI/CPack version. `LESHA_RC_VERSION` `5,0,3001,1`: Windows `.rc` files.
  - `LESHA_ANDROID_VERSION_CODE` = upstream code (with offset) * 100 + N, e.g. `217101`. `LESHA_APPLE_BUILD` = TWEAK*1000 + N, e.g. `1001`.
- Releasing: bump `LESHA_VERSION` in the PR. When it is merged, the push to `dev` builds everything and the release job creates tag `v<LESHA_FULL_VERSION>` and the GitHub Release itself; a `dev` push whose tag already exists publishes nothing.
- A hand-pushed tag still works and must be exactly `v<LESHA_FULL_VERSION>` (optionally `-rcN`/`-betaN` for a prerelease); the release job checks it against `CMakeLists.txt` and fails otherwise.
- Files: root `CMakeLists.txt`, `version.h.in`, `cmake/CPack.cmake`, `client/cmake/{android,ios,macos,macos_ne}.cmake`, both `networkextension/CMakeLists.txt`, `client/platforms/windows/amneziavpn.rc.in`, `service/server/amneziavpn-service.rc.in`, `client/main.cpp`, settings controllers (About page), `coordinatorApi.cpp`, `.github/workflows/deploy.yml`.
How to resolve: for `AMNEZIAVPN_VERSION` and `APP_ANDROID_VERSION_CODE` take **theirs**; keep the fork's `LESHA_*` block and the places that use `LESHA_*`/`APP_FULL_VERSION` instead of upstream's version vars; bump `LESHA_VERSION` by 1. If upstream adds a new place that stamps a version (a new platform plist, installer, manifest), point it at the `LESHA_*` values too.

## 9. Features cut out of the build
The fork only talks to the coordinator and only uses AmneziaWG, so features it can't reach are removed to make the
app smaller and the build faster. Files that only served a removed feature are deleted (`git rm`); upstream files
that referenced them carry small hunks. How to resolve, for every item below: if upstream modified a file the fork
deleted, keep it deleted (`git rm` it again) and drop upstream's change; in a hunk-carrying file take upstream's
version and remove the references to the deleted feature again. If upstream adds a new file that only serves a
removed feature, don't add it to the build.

### 9.1 QR-code import and gamepad navigation (Android)
- Deleted: `client/android/src/org/amnezia/vpn/CameraActivity.kt`, `client/android/res/layout/camera_preview.xml`,
  `client/ui/qml/Components/GamepadLoader.qml`, submodule `client/3rd/qtgamepad`.
- Hunks: `client/android/{AndroidManifest.xml,build.gradle.kts,gradle/libs.versions.toml}` (camera permission and
  features, CameraX and ML Kit), `AmneziaActivity.kt` / `AmneziaApplication.kt` / `qt/QtAndroidController.kt`
  (`isCameraPresent`, `startQrCodeReader`, CameraX config, `decodeQrCode`; the gamepad A/B/X/Y/Start/Select branch of
  `AmneziaActivity.dispatchKeyEvent` and its unimplemented `nativeGamepadKeyEvent`), `client/platforms/android/android_controller.{h,cpp}`
  (same JNI methods), `ui/controllers/importUiController.cpp`, `ui/controllers/settingsUiController.cpp`
  (`isCameraPresent()` is false on Android), `client/cmake/3rdparty.cmake` (qtgamepad block), `ui/qml/main2.qml`
  (gamepad `Loader`), `ui/qml/qml.qrc`.

### 9.1a Android "open with" config import
The app no longer registers as a handler for `vpn://` links, `.vpn`/`.cfg`/`.conf` files or shared text (configs only come
from the coordinator, and `vpn://` collided with the official AmneziaVPN app).
- Deleted: `client/android/src/org/amnezia/vpn/ImportConfigActivity.kt`.
- Hunks: `client/android/AndroidManifest.xml` (the `ImportConfigActivity` with its SEND/VIEW filters, and the
  `org.amnezia.vpn.IMPORT_CONFIG` filter of `AmneziaActivity`), `AmneziaActivity.kt` (`processIntent`),
  `qt/QtAndroidController.kt` (`onConfigImported`), `client/platforms/android/android_controller.{h,cpp}`
  (`onConfigImported`, `configImported`, `importConfigFromOutside`), `core/controllers/coreSignalHandlers.cpp`
  (`initAndroidConnectionHandler`: the import-from-outside handler). The launcher, QS-tile preferences filter, VPN service
  and tile are unchanged. iOS's `importConfigFromOutside` is left as is (iOS is not built).

### 9.2 Self-hosted server installation, server services and sharing
The app can no longer install or manage a server over SSH, add services (Tor website, SFTP, SOCKS5, MTProxy, Telemt,
TProxy, AmneziaDNS) or share access/configs with other users. Stored self-hosted (admin) servers still load, connect with the
client config they already hold, can be renamed and removed; the `SelfHostedAdmin` config type and every
`core/models/protocols/*ProtocolConfig` (part of the `ProtocolConfig` variant) are kept for that. libssh is no longer a dependency.
- Deleted: `client/core/utils/selfhosted/{sshClient,sshSession,scriptsRegistry}.*`, `client/core/installers/*`,
  `client/core/controllers/selfhosted/{installController,usersController,exportController}.*`,
  `client/ui/controllers/selfhosted/{installUiController,exportUiController}.*`, `client/server_scripts/` (with `serverScripts.qrc`),
  `client/ui/models/services/*`, `client/ui/models/clientManagementModel.*`, `client/ui/utils/mtProxyPublicHostInput.*`,
  `client/core/configurators/awgConfigurator.*` (AWG uses `WireguardConfigurator`), `recipes/libssh/`, `client/tests/selfHostedAdmin/`,
  QML `Pages2/{PageSetupWizardCredentials,PageSetupWizardEasy,PageSetupWizardInstalling,PageSetupWizardProtocols,PageSetupWizardProtocolSettings,
  PageDeinstalling,PageSettingsServerServices,PageService*,PageShare,PageShareConnection,PageShareFullAccess,PageProtocolAwgSettings,
  PageProtocolWireGuardSettings}.qml`, `Components/ConnectionTypeSelectionDrawer.qml`, `Controls2/CardType.qml`.
- Added: `client/ui/controllers/protocolSettingsUiController.{h,cpp}` (QML `ProtocolSettingsController`: `updateProtocols`,
  `openClientSettings`, `updateClientConfig` — the client-only part of the old `InstallController`).
- Hunks: `conanfile.py` (libssh), `client/cmake/3rdparty.cmake` (`find_package(libssh)`), `client/main.cpp` (`ssh_init`),
  `client/CMakeLists.txt` (`serverScripts.qrc`), `client/cmake/sources.cmake`, `client/core/configurators/*` (no `SshSession`,
  `createConfig` and `processConfigWithExportSettings` removed; only `processConfigWithLocalSettings` and
  `WireguardConfigurator::genClientKeys` remain), `core/controllers/connectionController.cpp` (`ConfiguratorBase::create`),
  `core/controllers/serversController.{h,cpp}` (`updateClientConfig`, `validateClientConfig` moved from `InstallController`),
  `core/controllers/coreController.{h,cpp}`, `core/controllers/coreSignalHandlers.{h,cpp}` (connect of a self-hosted/native server
  validates via `ServersController::validateClientConfig`, no SSH provisioning), `core/controllers/updateController.cpp`
  (read `:/client_scripts/mac_installer.sh` directly; the file is now deleted, §9.3), `ui/controllers/connectionUiController.{h,cpp}` (`noInstalledContainers`,
  `isRevokeBlockedDuringActiveConnection`), `ui/controllers/serversUiController.{h,cpp}` (`removeServerFinished`),
  `ui/controllers/settingsUiController.{h,cpp}` (`ipAddressRegExp`), `ui/controllers/qml/pageController.h` (passphrase, share and
  services signals), `ui/models/protocolsModel.{h,cpp}` (`serverProtocolPage` role), `ui/models/serversModel.{h,cpp}`,
  `core/models/serverDescription.h`, `amneziaApplication.cpp` (`PublicHostInputValidator`), `ui/utils/pageEnum.h`, `ui/qml/qml.qrc`,
  `ui/qml/main2.qml` (passphrase drawer), `ui/qml/Filters/ContainersModelFilters.qml` (installed VPN protocols only),
  `Pages2/PageStart.qml` (share tab — tab indices shifted, `InstallController` connections), `Pages2/{PageHome,PageSettingsServerInfo,
  PageSettingsServerData,PageSettingsServerProtocols,PageSettingsServerProtocol,PageProtocolRaw,PageSettingsDns,
  PageProtocolAwgClientSettings,PageProtocolWireGuardClientSettings}.qml`, `Components/{SettingsContainersListView,HomeContainersListView}.qml`,
  `client/tests/{CMakeLists.txt,utils/testCoreController.h}`, `.github/workflows/deploy.yml` (ctest `-E` list), `THIRD_PARTY_LICENSES.md`.
- `Pages2/PageProtocol{OpenVpnSettings,Xray*}.qml` (server-side settings) became unreachable here and were deleted with the
  protocols (§9.4).

### 9.3 Amnezia gateway (premium/free VPN, news, purchases), update checker and settings backup
The app no longer talks to Amnezia's gateway at all: no "VPN by Amnezia" (premium/free/trial) configs, country lists,
subscriptions, device lists, native config export, purchases (Android Play Billing), news, captcha, support links or
update checks (§4), and no settings backup/restore (the backup file carried gateway-era settings, and the coordinator
re-issues configs anyway). Gateway configs (`config_version` 1 = Telegram, 2 = AmneziaGateway) are rejected on import
(`ImportInvalidConfigError`, as are backup files) and skipped when stored ones load:
`serverConfigUtils::configTypeFromJson` returns `Invalid` for them. The `ConfigType` enum values are kept so the numbering
stays stable. The Android `oss` product flavor (dimension `billing`) is kept with no billing code, so the APK output paths and
`onlyOssRelease` (§5) keep working; the `play` flavor and the `:billing` module are gone.
- Deleted: `client/core/controllers/gatewayController.*`, `client/core/controllers/api/*` (news, servicesCatalog,
  storePurchase, subscription), `client/core/utils/api/*` (apiUtils, countryCatalog, gatewayPayloadBuilder),
  `client/core/utils/payloadSender.*`, `client/core/utils/constants/apiKeys.h`, `client/core/models/api/*` (apiConfig,
  apiV2ServerConfig, authData, legacyApiServerConfig), `client/core/controllers/updateController.*`,
  `client/ui/controllers/updateUiController.*`, `client/ui/controllers/api/*`, `client/ui/models/api/*`,
  `client/ui/models/newsModel.*`, `client/android/billing/`, `client/android/play/`,
  `client/android/oss/org/amnezia/vpn/BillingPaymentRepository.kt`, `client/android/src/org/amnezia/vpn/BillingRepository.kt`,
  `client/client_scripts/` (with `clientScripts.qrc`), `client/images/flagKit/` (with `flagKit.qrc`),
  `client/resources/countries.qrc` + `countries/regions.json`, `client/images/update/*`, now-unused
  `client/images/controls/{archive-restore,arrow-left-right,bug,chevron-up,download,external-link,eye,gauge,globe-2,headphones,
  help-circle,history,infinity,monitor,news,news-unread,refresh-cw,search,settings-news,share-2,smartphone,sort-desc,star,
  star-filled,tag,unread-dot}.svg`, tests `client/tests/api/`, `client/tests/{testBackupExportImport,testAndroidRestoreBackup}.cpp`,
  QML `Pages2/{PageSettingsApi*,PageSetupWizardApi*,PageSettingsNewsDetail,PageSettingsNewsNotifications,PageSettingsBackup,
  PageDevMenu,PageUpdate}.qml`, `Components/{AdLabel,BenefitRow,BenefitsPanel,CountriesEmptyState,CountryListStickyHeader,
  CountrySearchField,CountrySearchRow,CountrySectionHeader,CountryUseCaseChips,SortCountriesDrawer,SubscriptionExpiredDrawer,
  SubscriptionPlanCard,TermsAndPrivacyText,UpdateSupportDrawer}.qml`, `Config/CountryRegionNames.qml`,
  `Controls2/{BackdropBlurType,CaptchaDialogType,ChangelogSectionType,CollapsingHeaderType,LabelWithImageType,TagBadgeType,
  ToastType}.qml`, `Controls2/TextTypes/{AppCaptionTextType,AppDisplayTextType,AppH1TextType,AppH3EmphasizedTextType,
  AppH3TextType,AppSmallTextType,AppTextType}.qml`.
- Hunks: `client/CMakeLists.txt` (gateway endpoint/key defines `PROD_AGW_PUBLIC_KEY`, `*_S3_ENDPOINT`, `DEV_AGW_*`,
  `FREE_V2_ENDPOINT`, `PREM_V1_ENDPOINT`; flagKit/countries/clientScripts qrc), `client/cmake/branding/{common,ios}.cmake`,
  `client/core/utils/appUiConfig.h.in`, `client/cmake/sources.cmake`, `client/cmake/android.cmake` (`ANDROID_BUILD_PLAY`,
  `android_play_*` targets), `deploy/build.sh` (`--play`/`--apk`; the AAB step always uses the `aab` target),
  `client/android/{build.gradle.kts,settings.gradle.kts,gradle/libs.versions.toml}`, `AmneziaActivity.kt` (billing methods,
  `isPlay`, `getCountryCode`), `client/platforms/android/android_controller.{h,cpp}` (same JNI methods),
  `client/amneziaApplication.cpp` (`IsAppUpdatesEnabled`, `IsPlayBuild`, update check), `core/controllers/coreController.{h,cpp}`,
  `core/controllers/coreSignalHandlers.{h,cpp}` (country list, subscription, update, news, backup handlers; connect always
  validates the client config), `core/controllers/settingsController.{h,cpp}`, `core/repositories/secureAppSettingsRepository.{h,cpp}`
  (gateway endpoint, country sort/favorites, news, auto update, ad label, backup), `client/secureQSettings.{h,cpp}`
  (backup/restore; `"Conf/favoriteLocations"` dropped from `encryptedKeys`), `core/repositories/secureServersRepository.{h,cpp}`,
  `core/models/serverDescription.{h,cpp}`, `core/utils/serverConfigUtils.{h,cpp}`, `core/controllers/serversController.{h,cpp}`,
  `core/controllers/connectionController.{h,cpp}`, `client/vpnConnection.cpp` (api config cases),
  `core/controllers/selfhosted/importController.{h,cpp}`, `ui/controllers/{importUiController,connectionUiController,
  serversUiController,settingsUiController}.{h,cpp}`, `ui/controllers/qml/pageController.h`, `ui/models/serversModel.{h,cpp}`
  (gateway/subscription roles), `client/images/images.qrc`, `ui/utils/pageEnum.h`, `ui/qml/qml.qrc`, `ui/qml/Config/qmldir`,
  `ui/qml/main2.qml` (captcha, subscription-expired and unsupported-connect drawers), `Pages2/{PageHome,PageSettings,
  PageSettingsAbout,PageSettingsApplication,PageSettingsServersList,PageSettingsServerData,PageSettingsServerInfo,PageSettingsDns,
  PageSettingsSplitTunneling,PageStart,PageSetupWizardConfigSource}.qml`, `Components/ServersListView.qml`,
  `client/tests/{CMakeLists.txt,run_android_tests.sh,testSettingsSignals.cpp,testConfigFormatVersion.cpp,utils/testUtils.cpp,
  utils/testCoreController.h}`, `.claude/skills/sync-upstream/SKILL.md`.
- Kept on purpose: `core/utils/errorCodes.h` (the Billing codes stay, numbering matches Android `ErrorCode.kt`),
  `core/utils/constants/apiConstants.h` (used by the coordinator API), `SecureAppSettingsRepository::getInstallationUuid`, the iOS StoreKit/purchase code
  in `platforms/ios` (iOS is not built by the fork; remove it if iOS is ever enabled), `qrCodeUtils` + `qrcodegen`
  (`ImportController` uses `qrMagicCode` to parse QR chunks).

### 9.4 XRay, OpenVPN (with Cloak/ShadowSocks) and IKEv2 protocols
Only AmneziaWG and plain WireGuard remain (the coordinator issues AWG 3.1; WireGuard shares the implementation). This drops
the heavy runtime deps: conan `amnezia-xray-bindings`, `tun2socks`, `openvpn` (with its `lz4`/`lzo`/`libnl`/`libcap-ng`
deps), `v2ray-rules-dat` (geosite/geoip), `tap-windows6`, and on Android `amnezia-libxray`, `openvpn-pt-android`. The
service no longer ships `openvpn`, `tun2socks`, `geo*.dat` or the TAP driver, and the privileged-process IPC
(`IpcProcessInterface`, `createPrivilegedProcess`), which only launched openvpn/tun2socks/certutil, is gone.
The `DockerContainer`/`Proto` enum values stay (persisted). A stored server with an OpenVpn, Cloak, ShadowSocks, Ipsec, Xray
or SSXray container still loads: `ContainerUtils::isUnsupportedContainer` is true for all of them (connect fails with
`LegacyContainerNotSupportedError`, the protocol page says "no longer supported"), `defaultProtocol` maps them to
`Proto::Unknown` and their protocol data is not kept when the server is saved again. `.ovpn`, xray JSON and
`vless://`/`vmess://`/`trojan://`/`ss://`/`ssd://` imports are rejected as invalid; AWG/WireGuard `.conf` and `vpn://` keys import as before.
Android: a stored `VpnState` with a removed `VpnProto` fails to decode and resets to the default state (corruption handler).
- Deleted: `client/core/protocols/{xrayProtocol,openVpnProtocol,ikev2VpnProtocolWindows}.*`, `client/core/configurators/
  {xrayConfigurator,openVpnConfigurator,ikev2Configurator}.*`, `client/core/models/protocols/{xray,openVpn,ikev2}ProtocolConfig.*`,
  `client/core/utils/managementServer.*`, `client/core/utils/serialization/`, `client/3rd/QJsonStruct/`,
  `client/ui/models/protocols/{xrayConfigModel,xrayConfigSnapshotsModel,openvpnConfigModel,ikev2ConfigModel}.*`,
  QML `Pages2/{PageProtocolOpenVpnSettings,PageProtocolXray*}.qml`, `Components/TransportProtoSelector.qml`,
  `Controls2/{MinMaxRowType,TextAreaType}.qml`, `client/tests/testXraySerialization.cpp`,
  `service/server/{xray.*,xray_defs.h,tapcontroller_win.*}`, `ipc/{ipc_process_interface.rep,ipcserverprocess.*}`,
  `deploy/data/{linux,macos}/update-resolv-conf.sh`, Android modules `client/android/{openvpn,xray}/` and
  `src/org/amnezia/vpn/{OpenVpnService,XrayService}.kt`, recipes `recipes/{amnezia-libxray,amnezia-xray-bindings,openvpn,
  openvpn-pt-android,tap-windows6,tun2socks,v2ray-rules-dat,libcap-ng}/`, `docs/openVpn*.plantuml`.
- Hunks: `conanfile.py`, `service/server/CMakeLists.txt` (xray link, openvpn/tun2socks/geo-dat copy+install, openvpn rpath
  fixups, TAP driver, `rasapi32`), `ipc/{ipc.h,ipc_interface.rep,ipcserver.{h,cpp}}` (removed slots `createPrivilegedProcess`,
  `checkAndInstallDriver`, `getTapList`, `createTun`, `deleteTun`, `Start/StopRoutingIpv6`, `enablePeerTraffic`, `enableKillSwitch`,
  `update/restoreResolvers`, `xrayStart/Stop`; `PermittedProcess`/`sanitizeArguments`), `service/server/{localserver.cpp,
  killswitch.{h,cpp},router.{h,cpp},router_{linux,mac,win}.{h,cpp}}`, `client/platforms/linux/daemon/linuxfirewall.cpp`
  (`130.allowMarkedXray` anchor no longer installed, still uninstalled), `client/CMakeLists.txt` (process replica,
  update-resolv-conf install, `rasapi32`), `client/cmake/{sources,android}.cmake`, `client/core/utils/{ipcClient,utilities}.{h,cpp}`,
  `core/models/{protocolConfig,containerConfig}.{h,cpp}`, `core/utils/containers/containerUtils.cpp`, `core/protocols/{protocolUtils,
  vpnProtocol}.cpp`, `core/configurators/configuratorBase.cpp`, `core/utils/constants/{configKeys,protocolConstants}.h`,
  `core/utils/commonStructs.h` (`ConnectionSettings::isApiConfig`), `core/controllers/{connectionController,coreController,
  serversController}.*`, `core/controllers/selfhosted/importController.{h,cpp}` (no OpenVPN malicious-script warning),
  `core/models/serverDescription.cpp`, `core/repositories/secureAppSettingsRepository.{h,cpp}` (`Xray/savedConfigs`),
  `client/vpnConnection.cpp` (include), `ui/controllers/{importUiController,serversUiController}.*`,
  `ui/models/{containersModel,protocolsModel}.{h,cpp}` (`isIpsec`, `isOpenVpn`, `isXray` roles), `ui/utils/pageEnum.h`,
  `ui/qml/qml.qrc`, `Components/SettingsContainersListView.qml`, `Pages2/{PageSetupWizardViewConfig,PageSetupWizardConfigSource}.qml`,
  `client/macos/app/Info.plist.in` (`.ovpn` document type), `client/android/{AndroidManifest.xml,build.gradle.kts,
  settings.gradle.kts}`, `VpnProto.kt`, `AmneziaActivity.kt` (`loadLibs()` loaded `librsapss.so` from `openvpn-pt-android`;
  without that lib the app crashed on launch), `client/tests/{CMakeLists.txt,testConnection.cpp}`, `THIRD_PARTY_LICENSES.md`,
  `.claude/skills/sync-upstream/SKILL.md`.
- Kept on purpose: `errorCodes.h`/`errorStrings.cpp` (numbering, matches Android), the env-gated tests that import
  `THIRD_PARTY_XRAY_VPN_KEY`/`OPEN_VPN`/`CLOAK`/`SS` keys as plain servers (they only exercise the server list), the
  iOS/macOS Network Extension code (`platforms/ios/*`, `client/{ios,macos}/networkextension`, `ios_controller.mm`) and its
  conan requirements `hev-socks5-tunnel`/`openvpnadapter` with their recipes: NE is not built by the fork and still uses them,
  so it no longer compiles; strip it if iOS/NE is ever enabled. The macOS service group (`AMN_SERVICE_GROUP`) stays, the pf
  rules use it.

## 10. Disguised tunnel interface name
The desktop daemon names the WireGuard/AmneziaWG network interface after a branding constant that looks like
an ordinary NIC (default `enp0s9`) instead of `amn0`, so on-device software listing interfaces by name is less
likely to flag it as a VPN. Caveats (documented, not bugs): tools that inspect the interface *type* (`ip -d link`)
still see `wireguard`; macOS forces `utunN` (cannot be renamed); Android's tun name is system-assigned and the OS
always marks the connection as a VPN, so this applies to **Linux and Windows only**.
- `client/cmake/branding/common.cmake` — `CLIENT_TUNNEL_INTERFACE_NAME` (CACHE, default `enp0s9`, ASCII ≤15 chars).
- `version.h.in` — emits the `TUNNEL_INTERFACE_NAME` define.
- Linux: `client/daemon/wireguardutils.h` (`WG_INTERFACE`), `client/platforms/linux/daemon/{linuxroutemonitor,wireguardutilslinux}.cpp`,
  and `linuxfirewall.cpp` (the `amn0+` allow-rules now use the exact interface name — no `+` prefix — so a NIC-looking
  name can't match a real card by prefix). `iputilslinux.cpp` follows `WG_INTERFACE`.
- Windows: `client/platforms/windows/daemon/wireguardutilswindows.h` `s_interfaceName()` returns `TUNNEL_INTERFACE_NAME`
  (the visible Wintun adapter name). The tunnel Windows service keeps its own internal name (`TUNNEL_SERVICE_NAME`), unchanged.
  `windowstunnelservice.cpp` builds the UAPI pipe path (`\\.\pipe\ProtectedPrefix\Administrators\AmneziaWG\<tunnel>`)
  from `s_interfaceName()`; upstream hardcoded `...\AmneziaWG\AmneziaVPN`, which no longer matched, so the peer was never
  set and the app hung on "waiting for handshake".
How to resolve: keep ours. If upstream adds a new place that hardcodes `amn0` for the interface, point it at `WG_INTERFACE`/`TUNNEL_INTERFACE_NAME`.

## 11. Settings, About and tray trimmed
Noah, 2026-10-13.
- `client/ui/qml/Pages2/PageSettings.qml` — the "Servers" entry is removed from the list (the servers pages stay in the
  build, just unreachable from Settings). Titles: "%1 account" (`Qt.application.displayName`) and plain "About" (context `PageSettings`).
- `client/ui/qml/Pages2/PageCoordinatorConfigs.qml` — header "%1 account" with the display name.
- `client/ui/qml/Pages2/PageSettingsAbout.qml` — no donation header/paragraph, no Privacy Policy button; contacts are only
  Telegram (https://t.me/kiraind, the `@kiraind` handle is not translatable) and GitHub (https://github.com/lesha-vpn). The version line stays.
- `client/ui/qml/Pages2/PageSettingsLogging.qml` — log descriptions are "%1 logs" / "%1 service logs" with the display name instead of "AmneziaVPN(-service) logs".
- `client/ui/utils/systemTrayNotificationHandler.{h,cpp}` — the tray "Visit Website" action is removed.
How to resolve: take upstream's changes, then drop these entries again. New upstream contacts/links in About are dropped.

## 12. Android: always-on and QS tile follow config updates
Upstream's VPN service saves the config (`LAST_CONF` in `Prefs`) only when the app connects, and always-on / the QS tile
connect with that saved copy. After the coordinator replaced a config (e.g. the entry node moved) they kept using the dead
one until the user connected once from the app. Now every change of the default server or its config (`serverEdited`,
`serverAdded` as default, `defaultServerChanged`) pushes the freshly built connection config (same JSON as on connect,
split tunneling included) to the service without connecting.
- `core/controllers/coreSignalHandlers.cpp` (`initAndroidSettingsHandler`) → `ConnectionController::saveAndroidConfig`
  (`core/controllers/connectionController.{h,cpp}`, signal `saveAndroidConfigRequested`) → `VpnConnection::saveAndroidConfig`
  (`client/vpnConnection.{h,cpp}`) → `AndroidController::saveConfig` (`platforms/android/android_controller.{h,cpp}`) →
  `AmneziaActivity.saveVpnConfig` → service action `Action.SAVE_CONFIG` (`IpcMessage.kt`, `AmneziaVpnService.kt`).
- The service process writes the prefs itself (SharedPreferences are cached per process): the activity sends the message
  over its binding, or binds briefly (`BIND_AUTO_CREATE`) when it isn't bound; an unstarted service then stops again on unbind.
How to resolve: keep the hunks; if upstream changes how `connect()` / `LAST_CONF` work in the service, keep a way to
update the saved config without connecting.

## 13. Android: app split tunneling page is the checkbox list of installed apps
Upstream's page listed only the added apps (with trash buttons) and a "+" opened `InstalledAppsDrawer` to pick more. On
Android the page itself now lists every installed app with a checkbox; a toggle adds/removes the app from the current mode's
list right away. Order is fixed when the page opens (or the mode changes): checked apps, user apps, system apps, each
alphabetically (`QCollator`), so a toggled row stays in place until the page is reopened. Apps from the list that are no
longer installed are shown checked so they can be removed. Windows keeps upstream's file list.
- `ui/qml/Pages2/PageSettingsAppSplitTunneling.qml` (Android branch, `isAndroid`), `ui/qml/Components/InstalledAppsDrawer.qml`
  deleted (and from `qml.qrc`).
- `ui/models/installedAppsModel.{h,cpp}` rewritten (`setApps`, `updateModel(savedApps)`, `setAppSelected`, `group` role).
- `AppSplitTunnelingUiController::setPackageInList` / `getPackagesInList`, `AppSplitTunnelingController::removeApp(InstalledAppInfo)`.
- `android/src/org/amnezia/vpn/AppListProvider.kt`: `system` flag (no launcher entry, or preinstalled and never updated).
- Test: `tests/testUiAppSTModelAndController.cpp` (`testInstalledAppsCheckboxList`). Strings `Selected`, `Apps`, `System apps`.
How to resolve: keep the fork's page/model; if upstream changes the drawer or the model, port its fixes into the page.

## Translations
Fork UI strings are **English source text** in `qsTr()`/`tr()`, like upstream's, and every fork string has a finished
translation in all 12 shipped `client/translations/amneziavpn_*.ts` (ru_RU, uk_UA, es_ES, fr_FR, ar_EG, fa_IR, hi_IN, ko_KR,
my_MM, th_TH, ur_PK, zh_CN). The brand stays «Лёша VPN» in every language; prefer `%1` + `Qt.application.displayName` /
`DISPLAY_NAME` over a literal. Russian uses the fork's wording («конфиг», «» quotes, «вы»).
Fork entries in the `.ts` files: contexts `CoordinatorController`, `PageCoordinatorConfigs`, `PageCoordinatorLogin` (fork-only),
`ProtocolSettingsUiController`, `ServersUiController` (strings moved out of upstream's deleted `InstallUiController`, §9.2), and
fork strings added to the upstream contexts `PageSettings`, `PageSettingsAbout`, `PageSettingsApplication`, `PageSettingsLogging`,
`PageSettingsServerData`, `PageSetupWizardConfigSource`, `PageSettingsAppSplitTunneling`.
- A new or changed fork string must be added to **every** `.ts` in the same change (a QML string's context is the file's basename).
  The fork's `.ts` files are edited by hand / script: don't run a full `lupdate` over them, it would mark hundreds of strings
  of the features removed in §9 as vanished. To find untranslated fork strings, run `lupdate` over the current tree and over
  the upstream base into scratch `.ts` files and compare (strings in the current tree but not in the base, plus anything in
  fork-only files, must have a finished translation with the same `%N` placeholders in each language).
- How to resolve: on conflict take upstream's `.ts` files, then re-add the fork entries listed above (copy the fork's
  `<message>` blocks from the pre-merge `dev` version of each file). If upstream changed the source text of a string the fork
  also changed, keep the fork's source text and its entries.

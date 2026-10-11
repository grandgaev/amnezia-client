# HANDOFF: AmneziaVPN — routing profiles, switching on the fly, AWG upgrade, AWG 3.1 fixes

Ты продолжаешь большую задачу, начатую другим агентом. Этот документ — полный контекст.
Пользователь общается по-русски, отвечай ему по-русски. Код, комментарии, коммиты, PR — по-английски.

---

## 0. Где всё лежит

- Репозиторий (форк): `https://github.com/grandgaev/amnezia-client` (публичный).
- Upstream: `https://github.com/amnezia-vpn/amnezia-client`, ветка `dev`.
- Рабочая ветка: **`claude/sweet-babbage-3wth5m`**. Все коммиты и пуши — туда.
  - 19 коммитов поверх `upstream/dev`, ~166 файлов, +17k/−1.8k строк.
- Бэкенд-роутер — **не в submodule**, а патчем:
  `recipes/awg-go-src/patches/0001-routing-router-and-transport-fixes.patch`.
  - Это `git format-patch` одного коммита поверх тега `v3.1.20260828` репозитория
    `https://github.com/amnezia-vpn/amneziawg-go`.
  - Форкнуть amneziawg-go не удалось (не было прав), поэтому патчи доставляются conan-рецептами.
- Документы в репо:
  - `docs/routing/HANDOFF.md` — этот файл;
  - `docs/routing/ROUTING_CONTRACT.md` — контракт между клиентом, демоном и роутером: формат `routing_config`, UAPI-ключи;
  - `docs/routing/PR_DESCRIPTION.md` — текст будущего PR в upstream;
  - `docs/routing/user-profile-happ-deeplink.txt` — реальный Happ-профиль пользователя, на котором воспроизводится открытый баг;
  - `.github/router-e2e/` — e2e роутера (Linux netns и macOS);
  - `tools/macos-routing-diagnostics.sh` — сбор диагностики с Mac пользователя.
- PR в upstream ещё **не открыт**. Пользователь откроет сам, когда всё будет готово. Без его прямой просьбы PR не создавай.

### Подпись коммитов

Это требование прошлой сессии. Если у тебя своя политика атрибуции, следуй ей, но не вставляй идентификаторы моделей в код и в тексты PR.

---

## 1. Исходный запрос пользователя (суть, дословно по смыслу)

Работа в форке `grandgaev/amnezia-client`. Нужен готовый код для **Windows, Android, iOS, macOS** и текст PR для официального репозитория. Четыре направления.

**1. Профили маршрутизации как в клиенте Happ — вместо старого split tunneling.**
- Типы правил: `domain:`, `full:`, `keyword:`, `regexp:`, `geosite:`/`geoip:`, IP/CIDR, обычные текстовые списки.
- Импорт и экспорт профилей, совместимые с Happ: JSON, base64, `happ://routing/add|onadd/<base64>`. Обработку диплинков Happ в ОС **не регистрировать**.
- Per-app маршрутизация на Android и Windows.
- Профили можно редактировать во время подключения; изменения применяются после переподключения.
- Должно работать на всех протоколах; **AmneziaWG 3.1 — главный приоритет**.

**2. Переключение серверов и протоколов «на лету»** без ручного отключения. Убрать все необоснованные блокировки вида «нельзя сделать X, пока подключено».

**3. Обновление контейнера AWG 2.0 → 3.1 в один клик без потери пользователей.**
- Механизм должен быть generic, чтобы годился для будущих версий.

**4. Починить `ERR_SOCKET_NOT_CONNECTED`** при работе через AWG 3.1.

**Общие требования:**
- Всё должно работать с первого раза.
- Сначала macOS и iOS.

**Пожелания, высказанные позже:**
- Неподписанный `.app`/`.pkg` для установки на свой Mac (Apple Silicon M4, macOS 26.5.1).
- Тестовая сборка ставится **рядом** с оригинальным AmneziaVPN и его не трогает.
- Следить за CI и чинить ошибки.
- Все сценарии проверить **по-настоящему**, в реальном окружении, а не только тестами.
- Довести до идеала: «вылизано на 100%», работать долго, ничего не бросать.
- Активно использовать субагентов: параллелить исследование, проверку и ревью (подробнее в §8).

---

## 2. Что сделано (по направлениям)

### 2.1 Профили маршрутизации

**Модель и компилятор** — `client/core/models/routing/`:
- `routingProfile.*`
  - Модель профиля: proxy/direct/block × sites/IPs, `GlobalProxy`, `RouteOrder`, DNS remote/domestic (DoU/DoH), `DnsHosts`, URL geo-файлов.
  - Happ exchange JSON с регистронезависимыми ключами; bool-значения Happ записываются строками `"true"`/`"false"`.
  - `parseImportData`: JSON, base64, `happ://`.
  - Обычные списки (`looksLikeRule`); миграция старых site-списков.
- `geoData`: читает `geosite.dat`/`geoip.dat` (protobuf v2ray).
- `ipRangeSet`.
- `routingCompiler.*`:
  - `expand()`;
  - `routerConfig()` — JSON для роутера в amneziawg-go;
  - `applyToXrayConfig()` — для Xray/SS-Xray;
  - `ipRoutes()` — адресный fallback для OpenVPN/Cloak/SS/IKEv2.
  - Поля `CompileInput`:
    - `bypassMode`: `os` для NE/Android, `interface` для десктопного демона;
    - `tunnelHasIpv6`;
    - **`ipv6Blocked`** (новое; kill switch на macOS/Linux).

**Контроллер:** `client/core/controllers/routingController.*`.
- Хранение в `SecureAppSettingsRepository`: `routingProfiles`, `isRoutingEnabled`, `selectedRoutingProfileId`, `routingExcludedRoutes`, `serverRoutingOverrides`.
- Загрузка geo-файлов с выбором User-Agent.

**Применение при подключении:** `client/vpnConnection.cpp` → `appendRoutingConfig()`.
- **AWG/WG — режим «router»:**
  - в конфиг кладётся `routing_config`;
  - DNS ОС = VIP `198.18.0.53`;
  - VIP добавляется в `allowed_ips`.
- **Xray/SS-Xray:** профиль переписывается в routing/outbounds/dns конфига Xray.
- **Остальные протоколы:** `splitTunnelType`/`splitTunnelSites`; имена резолвятся при подключении (до 256 хостов, 4 с).

**Роутер в amneziawg-go** (пакет `router/` в патче):
- `router.go`: обёртка над TUN; `decide()`; `handleOutbound()`.
- `dns.go`:
  - перехват всего UDP/53 и TCP/53;
  - remote-резолвинг через туннель, direct — снаружи (DoU/DoH);
  - запоминание адресов из ответов («learned»);
  - `blockZero`: заблокированные имена получают `0.0.0.0`/`::`.
- `netstack.go`: прямой TCP терминируется в gVisor и заново открывается через bypass-сокет.
- `udp.go`, `icmp*.go`.
- `bypass*.go`:
  - macOS: `IP_BOUND_IF`/`IPV6_BOUND_IF`;
  - Linux: `SO_BINDTODEVICE` + fwmark;
  - Windows: `IP_UNICAST_IF`;
  - Android: `SetSocketProtector` → `VpnService.protect`.
- `matcher.go`, `config.go`, `doh.go`.
- Транспортные фиксы в том же патче (см. §2.4).
- UAPI-ключи (работают на уровне устройства): `routing_config_file`, `routing_bypass_ifindex`, `routing_bypass_ifindex6`, `routing_bypass_ifname`, `routing_bypass_fwmark`. Подробности — в `ROUTING_CONTRACT.md`.

**Сборка бэкендов (conan):**
- `recipes/awg-go-src` — пропатченный исходник;
- потребители: `recipes/awg-go` (десктопный бинарь), `recipes/awg-windows` (`tunnel.dll`), `recipes/awg-apple` (+ свой патч, `wgSetRoutingConfigFile`), `recipes/awg-android` (+ патч, JNI socket protector);
- подключение через `replace` в `go.mod` и `GOFLAGS=-mod=mod`.

**Десктопный демон:**
- `client/daemon/daemon.cpp`, `interfaceconfig.*`: парсинг `routingConfig`, файл `routing.json`, UAPI.
- macOS:
  - `client/platforms/macos/daemon/wireguardutilsmacos.cpp`: uplink берётся из `MacosRouteMonitor`, `updateRouting`, `defaultInterfaceChanged`, `setRouterFirewall`;
  - `macosfirewall.cpp`: якорь pf `150.allowExcludedApps` с правилом `pass out quick proto {tcp,udp} user root`.
- Linux: `wireguardutilslinux.cpp` (fwmark `0x3212`, ослабление `rp_filter`, таймер uplink 5 с) и `linuxfirewall.cpp`.
- Windows: route monitor и UAPI через pipe.
- `client/mozilla/localsocketcontroller.cpp` передаёт `routingConfig`.

**Apple NE / iOS:** `client/platforms/ios/*` + bridging headers; конфиг пишется в контейнер NE.

**Android:** `client/android/**`.
- `GoBackend.awgSetSocketProtector`;
- строка routing в `WireguardConfig`;
- per-app через существующие механизмы.

**UI (QML):**
- новые страницы `PageSettingsRouting*`, `ServerRouting*`, `RoutingReconnectNotice`, `StringListDropDownType`;
- поиск по категориям geosite/geoip.

**Тесты:**
- `client/tests/testRouting.cpp` — 20 кейсов: парсинг, Happ import/export, geo, компиляция, конфиги для всех семейств протоколов, миграция.

### 2.2 Переключение на лету

- `client/vpnConnection.cpp`:
  - `isActiveState`, `m_suppressDisconnected`, состояние `Reconnecting`;
  - Android: stop → ждать `DISCONNECTED` → connect (`switchAndroidTunnel`/`finishAndroidSwitch`/`cancelAndroidSwitch`);
  - iOS: stop-wait-restart в `IosController`.
- `client/core/controllers/coreSignalHandlers.cpp` — переподключение по событиям:
  - `userSelectionChanged`;
  - `containerConfigUpdated`;
  - `containerRemoved`;
  - `serviceConfigChanged`;
  - смена kill switch.
- `onPrepareConfigFailed`: старый туннель остаётся, UI показывает реальное состояние.
- Блокировки «нельзя при подключении» убраны во многих страницах QML:
  - деструктивные действия сначала отключают VPN;
  - reboot/unlink выполняются и только потом отключают.

### 2.3 Обновление контейнера (AWG 2.0 → 3.1)

- `client/core/controllers/selfhosted/installController.*` — `upgradeContainer(serverId, container, ContainerUpgradeMode)`:
  - режимы `RefreshSoftware` и `UpgradeProtocol`;
  - последовательность: snapshot файлов состояния → сборка нового образа → park старого контейнера (`-pre-upgrade`) → run → restore → verify (15 × 1.5 с) → commit;
  - откат на любом шаге; подхват «застрявшего» parked-контейнера.
- `client/core/utils/selfhosted/awgContainerUpgrade.*`:
  - `parseAwgInterfaceSnapshot`, `buildUpgradedAwgConfig`;
  - `reRenderAwgAdminClientConfig` — сохраняет строку DNS;
  - `flagClientsForConfigUpdate`.
- Остальные части:
  - `AwgServerConfig::normalizeRandomTrailersCombo`;
  - `generateAwgParameters` сделан static;
  - `exportController::reissueConnectionConfig`;
  - `NeedsConfigUpdateRole`;
  - коды ошибок 220 и 221.
- UI:
  - кнопка **Upgrade** и шторка с двумя опциями в `PageSettingsServerProtocol.qml`;
  - `PageContainerUpgrading.qml`;
  - кликабельное предупреждение на `PageHome.qml`;
  - кнопка «Update config» в `PageShare.qml`.
- Тест: `client/tests/testContainerUpgrade.cpp` (8 кейсов).
- **На реальном сервере НЕ запускалось ни разу.** Legacy-контейнер `Awg` (1.x, `wg0.conf`) не поддержан.

### 2.4 `ERR_SOCKET_NOT_CONNECTED` / AWG 3.1

**Фиксы в amneziawg-go** (в том же патче):
- data-пакеты принимались за handshake при RandomTrailers + диапазонных H1–H3 + неравных S;
- терялся первый пакет при S4 > 0;
- гонка в таймерах;
- окно UDP сбрасывалось из-за сравнения endpoint по указателю.

**Остальное:**
- Бэкенды подняты до upstream-релиза `3.1.20260828`.
- Десктоп: при смене сети или пробуждении AWG-туннель больше не пересоздаётся. Переподключение происходит только если за 20 с нет rx (`VpnConnection::onNetworkChanged`).

### 2.5 Сборка, CI, установка на Mac (сделано в последней сессии)

- **`.github/workflows/macos-unsigned.yml`:**
  - собирает неподписанный `.pkg` и `.app` (артефакты `AmneziaVPNTest-macos-unsigned-pkg` и `-app`);
  - smoke-тест: запускает приложение и сервис на Apple Silicon раннере;
  - compile-check без подписи для iOS (`macos-26`, Xcode 26.4) и macOS NE;
  - кэш conan (restore/save раздельно, с fallback по префиксу), ccache;
  - concurrency отдельно для каждой джобы.
- **Ad-hoc подпись** для сборки без сертификата: `cmake/util/codesign.cmake` → `codesign_adhoc_sign_tree`, хук в `cmake/sign_binaries.cmake`.
  - Подписываются все Mach-O, все файлы в `Contents/MacOS` (geosite.dat, pf, скрипты), затем бандлы изнутри наружу; после этого всё проверяется.
  - Без подписи на M-чипах приложение падало с `SIGKILL Code Signature Invalid`.
- `CPackOptions.cmake`: пустая identity больше не передаётся в `--sign`.
- `client/cmake/macos_ne.cmake`: `-no-codesign` для macdeployqt передаётся только на Qt ≥ 6.11.
- **Side-by-side установка** `-DAMNEZIA_INSTANCE_ID=Test` → `AmneziaVPNTest`. Отдельно от оригинала:
  - бандл и bundle id;
  - настройки, keychain, single-instance;
  - сервис и launchd label, IPC `local:AmneziaVpnIpcInterface-test`, сокет `/var/run/amneziavpn-test`;
  - каталоги routing и pf;
  - идентификатор pkg и скрипты установщика.
  - Скрипты тестовой копии не сбрасывают pf-якорь `amn` и группу `amnvpn`.
  - Обновления в тестовой копии выключены.
  - Изменения: корневой `CMakeLists.txt`, `client/cmake/macos.cmake`, `service/server/CMakeLists.txt`, `ipc/ipc.h`, `client/daemon/*`, `client/mozilla/localsocketcontroller.cpp`, `cmake/CPack.cmake`, `deploy/data/macos/post_uninstall.sh` (флаг `SHARED_CLEANUP`), `deploy/build.sh`.
  - Без флага сборка совпадает с upstream.
- **`.github/workflows/router-e2e-macos.yml`** + `.github/router-e2e/macos-router-e2e.sh`.
  - Пропатченный amneziawg-go запускается на utun macOS-раннера и настраивается через UAPI так же, как демон.
  - Проверки: DNS через роутер; прямой TCP по learned-адресу и по IP-правилу; IPv6; правило kill switch; 20 параллельных соединений.
  - Сейчас **зелёный**.
- **Linux e2e** в network namespaces: `.github/router-e2e/linux/` (см. README там). Сейчас 23 из 23.

**Deploy workflow (upstream `deploy.yml`) в форке:**
- Linux build и тесты — зелёные; Windows build — зелёный.
- Apple-джобы падают на установке сертификатов: секретов нет, это ожидаемо.
- Android-джобы висят в очереди: нужен self-hosted `android-runner`, которого нет. **Android ни разу не собирался в CI.**

---

## 3. ОТКРЫТЫЙ БАГ №1 (главный приоритет)

**Симптом (macOS 26.5.1, MacBook Air M4).**
- Сборка `AmneziaVPNTest` из коммита `37150b7`, протокол AWG, профиль из `docs/routing/user-profile-happ-deeplink.txt` («RedHeadVPN — умный роутинг»).
- **Все сайты из правил direct** сразу падают с `ERR_CONNECTION_REFUSED`.
- Сайты через VPN работают.

**Профиль:**
- `GlobalProxy=true`, `RouteOrder=block-direct-proxy`.
- Direct: `geosite:whitelist`, `geosite:private`, `domain:ru` и ~190 доменов.
- Block: `geosite:category-ads`, `win-spy`, `torrent`.
- DNS: domestic — DoU `8.8.8.8`, remote — DoU `1.1.1.1`.
- `DnsHosts`: `dns.google`, `cloudflare-dns.com`.
- geosite: `hydraponique/roscomvpn-geosite`; geoip: Loyalsoldier.

**Уже выяснено:**
1. Профиль через `parseImportData` + `mergeExchangeJson` + `RoutingCompiler` с настоящими geo-файлами компилируется правильно:
   - 743 block-домена, 693 direct;
   - блок-лист не пересекается с прямыми сайтами (`yandex.ru`, `vk.com`, `ozon.ru`, `mail.ru`…).
2. На macOS-раннере роутер (тот же патч) работает: DNS, прямой IPv4 TCP, работа с правилом kill switch (упрощённым), параллельные соединения.
3. Нашли и исправили (коммиты `31088e3`, `37150b7`):
   - роутер отдавал AAAA для прямых доменов при uplink без IPv6, браузер шёл по IPv6 и получал мгновенный отказ;
   - теперь IPv6 проверяется UDP-connect'ом через bypass-сокет, и AAAA подавляются;
   - при включённом kill switch (по умолчанию **включён**, `Conf/killSwitchEnabled` = true) выставляются `rejectDirectIpv6` и `rejectProxyIpv6`, потому что pf `250.blockIPv6` всё равно блокирует IPv6 приложений.
4. **Пользователь говорит, что это не помогло.** Значит, есть ещё причина, и она проявляется только в реальном приложении или на реальной машине.

**Отличия реальности от e2e (подозреваемые):**
- **a) Полный набор правил kill switch.** В CI проверялось только `pass quick user root` + блок для не-root. Реальные якоря:
  - `100.blockAll`, `110`, `150` (pass quick user root), `200.allowVPN`, `250.blockIPv6`, `300.allowLAN`, `310.blockDNS`;
  - `350.allowHnsd` с `block return out group amnhnsd`.
  - Важно: сервис запускается launchd с **`GroupName amnvpn`**, и amneziawg-go наследует gid.
  - Нужно проверить семантику `quick` внутри под-якоря `amn/150...` и что правило `user root` реально совпадает с сокетами amneziawg-go, запущенного демоном.
  - Шаблон `amn.400.allowPIA.conf` использует `group amnvpn`.
- **b) Маршруты.** Реальный демон ставит `0.0.0.0/1` и `128.0.0.0/1` через utun, плюс exclusion route к серверу. В CI роутились только отдельные хосты.
  - Проверить scoped default route при `IP_BOUND_IF` и macOS 26.
- **c) Определение uplink.**
  - `MacosRouteMonitor::defaultIfindexIpv4` асинхронный (`rtmFetchRoutes`); в `updateRouting` индекс может быть 0, потом приходит `defaultInterfaceChanged`.
  - Если индекс ошибочно указывает на utun, получается петля.
  - Лог «Configuring the router, uplink: X Y» / «Updating the router uplink».
- **d) Ответы `0.0.0.0` (`blockZero`).**
  - Если прямой домен ошибочно классифицирован как block, DNS отдаст `0.0.0.0` → мгновенный refused.
  - Проверить `dig @198.18.0.53 ya.ru` на машине пользователя.
  - Возможно, приложение скачало другой geosite или иначе распарсило `geosite:category-ads`.
- **e) Сторонний файрвол** (Little Snitch, LuLu): может отказывать неподписанному `amneziawg-go` в исходящих → мгновенный refused.
- **f)** Сборка не та или кэш DNS/браузера (менее вероятно).
- **g)** Что-то в связке демона и UAPI: `routing_config_file` для тестовой копии лежит в `/var/run/amneziawg/routing-test/`; права; порядок UAPI.
- **h) Логов нет.**
  - До `31088e3` ошибки прямых соединений логировались только в verbose.
  - Сейчас — `Errorf` с ограничением частоты.
  - Release-сборка демона не задаёт `LOG_LEVEL`; stdout amneziawg-go пишется в лог сервиса.
  - **Логирование должно быть включено в настройках приложения.**

**Первым делом:**
1. Попроси пользователя на подключённом VPN:
   - включить логирование в приложении (Settings → Logging);
   - запустить `sudo bash tools/macos-routing-diagnostics.sh AmneziaVPNTest ya.ru youtube.com`;
   - прислать файл с рабочего стола.
   - Плюс скриншот ошибки браузера и название браузера.
   - Спросить, стоит ли Little Snitch/LuLu, и включён ли kill switch.
2. Параллельно воспроизведи в CI максимально близко к реальности (см. §5). Цель — найти причину без пользователя.

---

## 4. Прочие известные пробелы и риски (всё довести)

1. **Android**: код (Kotlin, JNI protector, рецепт `awg-android` с патчем) никогда не собирался в NDK и не запускался.
   - Добавить сборку Android в CI на GitHub-hosted ubuntu (Qt for Android, NDK 27, conan).
   - Запустить на эмуляторе: Android emulator на Linux-раннере с KVM, `reactivecircus/android-emulator-runner`.
2. **Windows**: собирается, но ни разу не запускался.
   - Роутер с `IP_UNICAST_IF` и UAPI через pipe, per-app.
   - Запустить на windows-раннере: установить, поднять туннель, прогнать сценарии.
3. **iOS / macOS NE**: только compile-check без подписи. Логика NE (запись `routing_config`, stop-wait-restart) не исполнялась.
   - Запуск на симуляторе ограничен (NE в симуляторе не работает). Минимум — юнит-тесты и ревью; если пользователь даст Apple-аккаунт, то реальная проверка.
4. **Обновление контейнера** ни разу не запускалось против настоящего сервера.
   - Поднять сервер AmneziaVPN в Docker на Linux-раннере: self-hosted-установка идёт по SSH на хост с Docker, можно sshd + docker в CI.
   - Прогнать: установка AWG 2.0 → добавление клиентов → Upgrade → проверка, что ключи и пиры сохранены, клиенты подключаются; плюс откат при сбое.
5. **Переключение на лету**: unit/signal-тесты есть, реального переключения между двумя серверами не было.
6. **Per-app routing** (Android, Windows) — не проверялся вживую.
7. **Linux daemon**: e2e проверял роутер, но не полный путь через демон и kill switch.
8. Транспортные фиксы AWG 3.1 проверены воспроизведениями. Нужен прогон с реальным сервером AWG 3.1 под нагрузкой (iperf, браузер, долгий idle, смена сети).
9. Все `go test ./device/` в старой песочнице падали и **без** наших изменений (проблема окружения). Перепроверить в чистом окружении.

---

## 5. Как проверять «по-настоящему» (идеи стендов)

- **Linux-стенд (GitHub ubuntu-раннер, root есть).**
  - Поднять сервер AmneziaWG 3.1 (контейнер Amnezia или amneziawg-go в netns).
  - Собрать **реальный клиент и сервис** AmneziaVPN для Linux, подключиться, прогнать маршрутизацию (DNS, direct/proxy/block, kill switch, IPv6).
  - Клиенту нужен способ управлять подключением без GUI: добавь тестовый CLI-хук или используй IPC (`QRemoteObjects`, `local:AmneziaVpnIpcInterface*`).
  - Сетевые namespaces — как в `.github/router-e2e/linux`.
- **macOS-стенд (macos-15/26 раннеры arm64, sudo без пароля).**
  - Установить собранный `AmneziaVPNTest.pkg` (`sudo installer`), сервис стартует через launchd.
  - Сервер AWG можно поднять **на том же раннере**: второй экземпляр amneziawg-go на другом utun с NAT через pf (`nat on en0`), endpoint `127.0.0.1`. Тогда можно ставить `0.0.0.0/1`+`128.0.0.0/1` без потери связи раннера, если добавить исключения для адресов GitHub.
  - Альтернатива: попросить у пользователя тестовый сервер или ключ.
  - Проверить с **полным** набором pf-якорей и включённым kill switch.
  - Внимание: `100.blockAll` рвёт связь раннера; держи окна короткими или используй allowNets для адресов раннера.
- **Android:** эмулятор на ubuntu-раннере с KVM; APK; подключение к серверу из Linux-стенда (через `adb reverse` или публичный адрес раннера недоступен — сервер внутри эмулятора/хоста).
- **Windows:** windows-раннер, установщик IFW/WiX, сервис, туннель.
- **Реальная машина пользователя** — финальная проверка. Всегда давай ему конкретные команды и ссылки на артефакты.

---

## 6. Как собирать и тестировать

**Linux, клиент и тесты:**
- Qt 6.10.1 (`aqt install-qt linux desktop 6.10.1 linux_gcc_64 -m qtremoteobjects qt5compat qtshadertools`), conan 2.28, gcc ≥ 11, ninja.
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt>/gcc_64 -DAMNEZIA_BUILD_TESTS=ON
ninja -C build AmneziaVPN AmneziaVPN-service test_routing test_container_upgrade
QT_QPA_PLATFORM=offscreen build/client/tests/test_routing   # аналогично другие тесты, ctest
```
- Первый configure сам вызывает `conan install`: сборка зависимостей из рецептов, долго.

**Роутер (Go):**
```
git clone --depth 1 --branch v3.1.20260828 https://github.com/amnezia-vpn/amneziawg-go.git awg
git -C awg checkout -b feature/routing
git -C awg am < recipes/awg-go-src/patches/0001-routing-router-and-transport-fixes.patch   # сохраняет коммит
cd awg && GOFLAGS=-mod=mod go test -race ./router/ && go vet ./router/
# для проверки на всех ОС:
GOOS=darwin GOARCH=arm64 go vet ./router/
GOOS=windows go vet ./router/
GOOS=android GOARCH=arm64 go vet ./router/
```
- После правок: `git commit --amend`, затем `git format-patch -1 HEAD --stdout > <repo>/recipes/awg-go-src/patches/0001-routing-router-and-transport-fixes.patch`.
- Изменение рецепта меняет ключ conan-кэша в CI, поэтому пересоберутся бэкенды.
- Рецепты apple и android имеют **свои** патчи (`recipes/awg-apple/patches`, `recipes/awg-android/patches`). Если меняешь API роутера, проверь их.

**CI в форке:**
- Пуш в `claude/**` запускает `macos-unsigned.yml`, `router-e2e-macos.yml` (при изменении `recipes/awg-go-src/**` или `.github/router-e2e/**`) и `deploy.yml`.
- Артефакты: `https://github.com/grandgaev/amnezia-client/actions/runs/<run>/artifacts/<id>`. Пользователь качает их, будучи залогиненным на GitHub.

**Установка тестовой копии на Mac:**
```
xattr -dr com.apple.quarantine AmneziaVPNTest_*.pkg
sudo installer -pkg AmneziaVPNTest_*.pkg -target /
```
- Получается `/Applications/AmneziaVPNTest.app`, сервис `AmneziaVPNTest-service`, launchd `/Library/LaunchDaemons/AmneziaVPNTest.plist`, лог сервиса `/var/log/AmneziaVPNTest/`.

---

## 7. Грабли, на которые уже наступали

- macOS arm64: без валидной подписи → `SIGKILL Code Signature Invalid`. macdeployqt и strip портят подпись линкера.
  - codesign считает **всё** в `Contents/MacOS` вложенным кодом.
  - Путь к главному исполняемому файлу подписывает весь бандл.
- Qt 6.10 macdeployqt: нет `-no-codesign`, подписи по умолчанию тоже нет. В Qt 6.11+ наоборот.
- CPack-скрипты выполняются без политик по умолчанию (`cmake_policy` в `cmake/util/codesign.cmake`).
- Upstream `post_uninstall.sh` выполняется **как preflight** при каждой установке:
  - сбрасывает все pf-якоря `amn`;
  - может удалить группу `amnvpn`.
- Kill switch по умолчанию включён и блокирует весь IPv6 приложений (`block return`).
- Роутер отвечает `0.0.0.0`/`::` для block-доменов. Мгновенный refused на сайте означает, что надо проверить классификацию.
- `amneziawg-go` в release логирует только ошибки; демон пишет stdout процесса в лог сервиса.
- В bash-скриптах CI `wait` без аргументов ждёт и фоновые демоны (так уже было зависание).
- Upstream-скрипт `deploy/build_macos_ne.sh`, на который ссылается `deploy.yml`, отсутствует; macOS NE собирается через `deploy/build.sh` или cmake.
- В `deploy.yml` есть Android-джобы на self-hosted раннере: они висят в очереди. Устаревшие прогоны можно отменять.

---

## 8. Как работать (обязательный workflow)

**Фаза 0 — ориентация (параллельно субагентами):**
- Субагент A: прочитать diff ветки против `upstream/dev` по клиенту и QML, составить карту фич и рисков.
- Субагент B: роутер (патч) — код, тесты, сборка на всех ОС.
- Субагент C: CI (workflows, рецепты), состояние последних прогонов.
- Субагент D: подготовить диагностику для пользователя (§3) и стенды (§5).
- Ты — координатор: держишь план и чек-лист, сам принимаешь решения и делаешь финальные правки.

**Фаза 1 — баг №1 (direct refused на macOS).**
- Получить диагностику пользователя и одновременно собрать CI-стенд с **реальным** приложением и полным kill switch.
- Сначала воспроизвести, потом чинить, потом проверить на стенде, потом на машине пользователя.
- Не объявлять починенным без подтверждения пользователя или воспроизводящего стенда.

**Фаза 2 — реальные стенды для каждой платформы (§5).** Матрица сценариев, каждый прогоняется реально:

| Область | Сценарии |
|---|---|
| Маршрутизация | direct, proxy, block по domain/full/keyword/regexp/geosite/geoip/CIDR; RouteOrder; GlobalProxy true/false; DNS (DoU/DoH, hosts); IPv4/IPv6 uplink; kill switch on/off; Wi-Fi ↔ Ethernet; сон/пробуждение |
| Протоколы | AWG 3.1 (приоритет), AWG 2.0, WireGuard, Xray/VLESS, SS-Xray, OpenVPN/Cloak/SS (адресный fallback) |
| Импорт/экспорт | Happ JSON, base64, `happ://`, текстовые списки, обмен с Happ туда и обратно |
| Per-app | Android, Windows |
| Переключение на лету | сервер ↔ сервер, протокол ↔ протокол, смена профиля с переподключением, отказ подготовки конфига |
| Обновление контейнера | 2.0 → 3.1 с пользователями, RefreshSoftware, откат, прерванный апгрейд, Update config для клиентов |
| `ERR_SOCKET_NOT_CONNECTED` | долгие соединения, idle, смена сети, нагрузка, RandomTrailers-комбинации |

**Фаза 3 — ревью и полировка.**
- Субагенты делают независимое code review (корректность, гонки, утечки, UI/UX, переводы, a11y).
- Ты исправляешь найденное. Повторять до чистого результата.

**Фаза 4 — CI зелёный на всех платформах.**
- Android-сборка добавлена; Windows/Linux/macOS/iOS проходят.
- Обновить `docs/routing/PR_DESCRIPTION.md` фактами о реальных проверках.

**Правила:**
- Каждое утверждение «работает» подкреплено реальным прогоном (лог или артефакт). Иначе прямо пиши «не проверено».
- Маленькие проверенные коммиты, с понятными сообщениями. Пушить в `claude/sweet-babbage-3wth5m`.
- Перед пушем: локальные тесты, `go vet` для всех ОС, сборка.
- Регулярно коротко сообщай пользователю статус по-русски. Ссылки на артефакты — прямые.
- PR в upstream не создавать без явной просьбы.

---

## 9. Definition of Done

- [ ] Баг №1 исправлен и подтверждён на Mac пользователя.
- [ ] Все сценарии §8 прогнаны на реальных стендах; результаты в отчёте.
- [ ] Android собирается в CI и работает на эмуляторе (маршрутизация, per-app, переключение).
- [ ] Windows установлен и проверен на раннере.
- [ ] Обновление контейнера проверено на реальном сервере с пользователями, включая откат.
- [ ] Все тесты зелёные, CI зелёный (кроме шагов, требующих секретов upstream).
- [ ] `PR_DESCRIPTION.md` обновлён; ветка готова к PR.

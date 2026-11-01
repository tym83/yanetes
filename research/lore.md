# Яnetes — фактура и мемы

Собрано 2026-10-30. Метки: **FACT** — подтверждено источником; **MEME** — устойчивая шутка или пользовательский опыт (источник — пример, а не доказательство «официальности»). Всё, что не удалось подтвердить, вынесено в конец, в раздел «Не подтверждено — не использовать как факт».

---

## 1. NIH: всё своё

### 1.1 Arc / Arcadia вместо git
- **FACT.** У Яндекса монорепозиторий Arcadia и своя VCS Arc. Данные доклада 2020 года: «6 млн коммитов, почти 2 млн отдельных файлов», вся история весит около 2 ТБ. Репозиторий исторически жил в SVN (trunk-based), переход на Mercurial не дотянул по скорости, git-svn за время подготовки доклада смог сконвертировать только кусок истории. Рабочая копия — виртуальная ФС (FUSE / ProjFS) с ленивой подгрузкой файлов. На момент доклада Arc пользовались около 20% разработчиков, задача — «убедить остальные 80%».
  Источник: [Habr, «Arc — система контроля версий для монорепозитория. Доклад Яндекса», 2020](https://habr.com/ru/post/482926)
- → в k8s: `kubectl apply` не принимает git-URL, только `arc://`. GitOps-контроллер `YaFlux` синкается из Arcadia; ConfigMap монтируется через FUSE и подгружает ключи лениво — при первом чтении.

### 1.2 ya make (yatool)
- **FACT.** `ya make` — своя система сборки для монорепо, открыта как [github.com/yandex/yatool](https://github.com/yandex/yatool) (Apache-2.0). В README прямо сказано, что сборка «сильно интегрирована во внутреннюю экосистему», а документация на русском. CatBoost в итоге ушёл с Ya Make на CMake: [CatBoost docs](https://catboost.ai/docs/en/concepts/build-from-source).
- → в k8s: образы собираются только через `ya make --docker`; Dockerfile отклоняется admission-вебхуком с сообщением «используйте ya.make».

### 1.3 Porto вместо Docker
- **FACT.** Porto — собственная система управления Linux-контейнерами (cgroups/namespaces, protobuf API поверх unix-сокета, CLI `portoctl`). Из вакансии Яндекса: «все контейнеры облака Яндекса основаны на Porto», есть шим для CRI.
  Источники: [AUR: porto, upstream github.com/yandex/porto](https://aur.archlinux.org/packages/porto), [вакансия Яндекса](https://yandex.com.tr/jobs/vacancies/system-developer-internal-cloud-14505)
- → в k8s: CRI по умолчанию — `porto-shim`; `crictl` переименован в `portoctl`; containerd в образе ноды остаётся, но в «режиме совместимости».

### 1.4 YP (Yandex Planner) вместо Kubernetes
- **FACT.** YP управляет более чем 75% вычислительных ресурсов Яндекса: свыше 50 тыс. хостов и свыше 2 млн ядер, пиковая номинальная утилизация 97%. Главный аргумент против k8s — масштаб: k8s официально поддерживает около 5000 нод, а самый большой кластер YP — «чуть более 30 тысяч хостов». Вместо etcd — YT (динамические таблицы + Cypress). Эвикция двухфазная: YP просит, внешний Yandex Deploy подтверждает (обычно 10–30 минут). Дефрагментация решается как ILP через Google OR-Tools/CBC, по оценке автора экономит «десятки миллионов долларов». Цитата: «Мы не можем предсказывать будущее».
  Источник: [Habr, блог Яндекса, «Yandex Planner. Как планировать вычислительные мощности», 2022](https://habr.com/ru/post/564510)
- → в k8s: etcd заменён на YT; `kubectl` предупреждает «ваш кластер меньше 30 000 нод, это не продакшн»; `kubectl drain` создаёт EvictionRequest и ждёт 10–30 минут подтверждения от внешнего контроллера; планировщик решает ILP и пишет в Events «экономия: $10M».

### 1.5 Yandex Cloud продаёт… Managed Kubernetes
- **FACT.** Внутри Яндекса работает YP, а наружу Yandex.Cloud продаёт Managed Service for Kubernetes (preview с мая 2019).
  Источник: [блог Yandex Cloud, 2019](https://yandex.cloud/ru/blog/posts/2019/05/managed-kubernetes)
- → в k8s: в README дистрибутива — «Kubernetes мы продаём клиентам, а сами пользуемся Яnetes».

### 1.6 YT / YTsaurus, YDB, ClickHouse — свои базы
- **FACT.** YTsaurus открыт в 2023 году под Apache 2.0; по данным ТАСС, развёрнут на «десятках тысяч серверов» и обрабатывает эксабайты данных. [TASS](https://tass.com/economy/1591543/amp)
- **FACT.** YDB открыта в апреле 2022 года, Apache 2.0. [Yandex Cloud blog](https://yandex.cloud/ru/blog/posts/2022/04/ydb-available-in-open-source), [Wikipedia](https://en.wikipedia.org/wiki/YDB_(database))
- **FACT.** ClickHouse вырос из Яндекс.Метрики (прототип в 2009 году), открыт в июне 2016-го, в сентябре 2021-го выделен в ClickHouse, Inc. [пресс-релиз Яндекса](https://yandex.com/company/press_center/press_releases/2021/2021-09-20)
- → в k8s: backend состояния кластера на выбор: YT, YDB или ClickHouse, etcd — «только для тестов». Это хороший пример, где NIH оказался прав, — можно подать как самоиронию.

### 1.7 Odyssey вместо PgBouncer, userver
- **FACT.** Odyssey — свой пулер соединений для PostgreSQL. Цитата: «Раньше у нас был PgBouncer, но с ним был ряд проблем… pull request'ы не были приняты». [tproger](https://tproger.ru/news/odyssey-postgresql-open-source)
- **FACT.** userver — свой асинхронный C++-фреймворк. Родился в Такси, открыт под Apache 2.0. [Rozetked](https://rozetked.me/news/24771-yandeks-vylozhil-v-otkrytyy-dostup-ishodnyy-kod-userver)
- → в k8s: «апстрим не принял PR → форкнули» как официальный процесс. Например, `CONTRIBUTING.md`: «если апстрим не мержит за 7 дней, создаётся `ya-<project>`».

### 1.8 Трекер вместо Jira
- **FACT.** Яндекс.Трекер изначально был внутренним инструментом, потом его открыли внешним клиентам. По CNews, свой трекер сделали, потому что готовые решения не держали нагрузку.
  Источники: [Habr, «Как сделать внутренний продукт внешним»](https://habr.com/ru/companies/yandex/articles/345044), [CNews](https://zoom.cnews.ru/news/item/368221)
- → в k8s: issue-ссылки в аннотациях только формата `QUEUE-12345`; на любой Jira-URL — warning.

### 1.9 Яндекс.Браузер — «свой» браузер на Chromium
- **FACT.** Представлен 1 октября 2012 года на YaC. До него были «Яндекс.Хром» (2010) и «Яндекс.Интернет» — сборки Chromium. Волож: «построен на открытой платформе Webkit и надстройке Chromium».
  Источники: [Wikipedia](https://en.wikipedia.org/wiki/Yandex_Browser), [Ведомости, 2012](https://www.vedomosti.ru/technology/articles/2012/10/01/yandeks_zapustil_svoj_brauzer)
- → в k8s: дистрибутив — форк upstream с переименованными бинарями (`ya-kubelet`, `ya-apiserver`), при этом в `--version` честно написано «based on Kubernetes».

### 1.10 SmartCaptcha вместо reCAPTCHA, Мессенджер, Телемост
- **FACT.** SmartCaptcha позиционируется как замена reCAPTCHA. [Skillbox Media](https://skillbox.ru/media/code/yandeks-vypustil-svoy-analog-kapchi-dlya-zashchity-ot-ddosatak-i-spama-smartcaptcha/)
- **FACT.** Яндекс Мессенджер объединён с Телемостом: чаты и звонки теперь в одном приложении. [comss.ru](https://www.comss.ru/page.php?id=22092)

### 1.11 Утечка Arcadia, 2023
- **FACT.** В январе 2023 года утекло около 45 ГБ кода из внутреннего репозитория, включая файл `factors_gen.txt` с 1922 факторами ранжирования. Яндекс заявил, что утекла устаревшая версия.
  Источники: [dev.by](https://devby.io/news/seichas-nam-ochen-stydno-chto-uzhe-izvestno-iz-utechki-koda-yandeksa), [Habr, Людкевич](https://habr.com/ru/post/718678), [Techzine](https://www.techzine.eu/news/security/101200/massive-leak-of-yandex-code-reveals-1922-search-ranking-factors/)
- → в k8s: у ya-scheduler ровно 1922 scoring-плагина (`factors_gen.txt` лежит в ConfigMap).

---

## 2. Масштаб и highload

- **FACT.** Самый большой кластер YP — около 30 тыс. хостов против примерно 5000 в официальном лимите k8s (см. 1.4). Это ядро шутки «scale nobody else has». [Habr](https://habr.com/ru/post/564510)
- **FACT.** Яндекс.Метрика — более 1,6 млн RPS и более 100 млрд событий в сутки (доклад 2022 года на YDB Meetup). [presentations.ydb.tech](https://presentations.ydb.tech/2022/ru/highload_foundation_million_rps_metrika/presentation.pdf)
- **FACT.** 5 сентября 2021 года ботнет Mēris атаковал Яндекс с пиком 21,8 млн RPS — на тот момент рекорд L7-атаки. [Habr, Qrator](https://habr.com/ru/companies/qrator/articles/577026/), [BleepingComputer](https://bleepingcomputer.com/news/security/new-m-ris-botnet-breaks-ddos-record-with-218-million-rps-attack/)
- **FACT.** На странице об архитектуре поиска Яндекс пишет о «кластерах кластеров». [yandex.ru/company/technologies/searcharch](https://yandex.ru/company/technologies/searcharch)
- **FACT.** 19 августа 2011 года Яндекс лежал около трёх часов из-за ошибки маршрутизатора в новом дата-центре в Амстердаме. [Lenta.ru](https://lenta.ru/articles/2011/08/20/yandex/)
- **FACT.** Утилизация YP на пике — 97%. [Habr](https://habr.com/ru/post/564510)
- → в k8s: стандартный `rateLimit` API-сервера — 21.8M RPS («меньше — значит, вас ещё не DDoS-или»); HPA по умолчанию с `minReplicas: 30000`; таргет утилизации в ResourceQuota — 97%; объект `ClusterOfClusters`.

---

## 3. Найм

- **FACT.** Официальный FAQ Яндекса: после отказа на интервью «мы советуем вам взять паузу примерно в полгода», а в зависимости от подразделения — до года. Фидбек — «как правило, три дня». На другую вакансию после отказа откликаться можно.
  Источник: [yandex.ru/jobs/interview/faq](https://yandex.ru/jobs/interview/faq/)
- **FACT.** Формат: алгоритмические секции (код вживую), архитектурная секция для senior и тимлидов, примерно с 2024 года — секция Advanced Code (час, IDE, демонстрация экрана). [Habr, 2025–26](https://habr.com/ru/articles/1006022/)
- **FACT.** Быстрый найм: Weekend Offer, Fast Track, Intern Week Offer — «оффер за 1–2 дня». [yandex.ru/jobs/hiring-events](https://yandex.ru/jobs/hiring-events/index), [Rozetked](https://rozetked.me/news/25581-yandeks-sozdal-kalendar-bystrogo-nayma)
- **MEME.** Кандидат проходит все секции, после чего тишина или отказ; «собеседование в Яндекс — больше никогда».
  Примеры: [vc.ru, «Собеседование в Яндекс? Больше никогда»](https://vc.ru/1240599-sobesedovanie-v-yandeks-bolshe-nikogda), [Habr: дошёл до финала, а сообщить «забыли»](https://habr.com/en/articles/855296/comments)
- **FACT.** «Яндекс — это школа»: Школа анализа данных (ШАД) работает с 2007 года, бесплатно, два года обучения, выпустила более 1400 человек. [Rozetked](https://rozetked.me/news/33712-sostoyalsya-16-y-vypusk-shkoly-analiza-dannyh-yandeksa)
- → в k8s:
  - Pod проходит «секции» — цепочку из 4–5 readinessGate'ов (`algo-1`, `algo-2`, `architecture`, `team-match`).
  - После провала Pod попадает в backoff ровно на 6 месяцев (`restartPolicy: AfterHalfYear`), в некоторых namespace — на 12.
  - Состояние `Pending` после прохождения всех gate'ов — reason `TeamMatching`.
  - `WeekendOffer` — режим, в котором gate'ы проходят за 48 часов.

---

## 4. Яндекс Плюс

- **FACT.** Отмена подписки в 2024 году заняла 8 экранов. На пути: «добавьте семью», «баллы сгорят» (мелким шрифтом — через 30 дней), список контента без кнопки отмены (выход только через «Не интересно»), скидка за то, чтобы остаться (199 ₽ за три месяца вместо 299 ₽ в месяц), экран с устройствами пользователя. Кнопки: «Пусть баллы сгорают», «Всё равно отменить». На финальном экране — «Возобновить Плюс».
  Источник: [iPhones.ru, 20.02.2024](https://www.iphones.ru/iNotes/poproboval-otmenit-podpisku-yandeks-plyus-eto-zhest-kakaya-to-02-20-2024)
- **FACT.** Гайды пишут, что кнопка отмены — в самом низу страницы, а подписку по промокоду к устройству отменить можно не всегда. [iGuides](https://www.iguides.ru/main/os/kak_otklyuchit_podpisku_plyus_ot_yandeksa_neskolko_proverennykh_sposobov/)
- **FACT.** В ноябре 2023 года ФАС возбудила дело против ООО «Яндекс» за рекламу Плюса со скидкой 44%, которую нельзя было получить (недостоверная цена, штраф до 500 тыс. ₽). Это дело о рекламе, а не об отписке. [Habr](https://habr.com/en/news/775020), [iPhones.ru](https://www.iphones.ru/iNotes/fas-zavela-delo-protiv-yandeksa-iz-za-nedostovernoy-reklamy-plyusa-11-20-2023)
- **FACT.** Закон 376-ФЗ от 15.10.2025 (в силе с 1 марта 2026 года): для автосписаний нужно явное согласие. Источник вторичный, дату стоит сверить на pravo.gov.ru. [Т—Ж / Т-Банк Секреты](https://secrets.tbank.ru/blogi-kompanij/izmenenie-pravil-prodleniya-podpisok/)
- **FACT.** Кешбэк начисляется баллами (1 балл = 1 ₽), копить их можно только при активной подписке, тратить — на саму подписку и в «Плюс Сити». [yandex.ru/support/pro-card/ru/cashback](https://yandex.ru/support/pro-card/ru/cashback)
- **FACT.** Цены: с 1 декабря 2025 года — 449 ₽ в месяц (было 399 ₽), с 16 июня 2026 года годовая подписка — 4490 ₽ (было 3490 ₽). [vbr.ru](https://www.vbr.ru/novosti/kursy-valyut/2025/11/05/yandeks-plyus-podorojaet-s-dekabrya/), [comss.ru](https://www.comss.ru/page.php?id=20592)
- **MEME / пользовательский опыт.** Автопродление после пробного периода, а поддержка отвечает «доступ считается оказанным». [Harant](https://harant.ru/questions/q-227303/)
- → в k8s:
  - Финализатор `plus.yandex.ru/retention` на каждом namespace. `kubectl delete ns` проходит 8 стадий подтверждения (`--i-want-to-cancel`, `--let-points-burn`, `--not-interested`, `--cancel-anyway`); на шестой предлагается скидка на ResourceQuota.
  - Mutating webhook вставляет sidecar `plus-upsell` в каждый Pod.
  - CPU-кешбэк начисляется баллами, которые тратятся только на продление подписки на CPU.
  - Kubernetes 1.36 (март 2026) соответствует 376-ФЗ: добавлен флаг `--explicit-consent`.

---

## 5. Поисковые фильтры и алгоритмы

- **FACT.** Алгоритмы называли именами городов, причём каждое название начиналось с последней буквы предыдущего: Магадан (май 2008) → Находка → Снежинск (2009, MatrixNet) → …
  Источники: [пресс-релиз Яндекса, 2008](https://yandex.ru/company/news/2008-0516), [Skillbox](https://skillbox.ru/media/marketing/u-yandeksa-pochti-30-poimenovannykh-algoritmov-rasskazali-obo-vsekh-v-odnoy-state/)
- **FACT.** АГС-17 / АГС-30 — фильтры против дорвеев и «сателлитов»; название совпадает с автоматическими гранатомётами. [Habr](https://habr.com/en/articles/87018)
- **FACT.** «Минусинск» (2015) понижает сайты за покупные SEO-ссылки. [ruward](https://ruward.ru/cases/1186/), [postium](https://postium.ru/algoritm-yandeksa-minusinsk/)
- **FACT.** «Баден-Баден» (март 2017) — фильтр за переоптимизированные тексты; может применяться к отдельным страницам. [postium](https://postium.ru/algoritm-yandeksa-baden-baden/)
- **FACT.** «Палех» (2016) — нейросеть, сопоставляющая запрос и заголовки; «Королёв» (2017) — то же, но по всему тексту; «Вега» (2019) — комплексное обновление. Это не наказания. [postium Палех](https://postium.ru/novyj-algoritm-yandeksa-palex/), [postium Королёв](https://postium.ru/algoritm-yandeks-korolyov/)
- **FACT.** YATI (YaC 2020) — трансформер. Яндекс назвал его важнейшим изменением поиска за 10 лет, модель в «несколько миллионов раз сложнее» предыдущей. [Yandex blog](https://yandex.com/company/blog/yet-another-conversation-2020-yandex-talks-about-what-matters-and-how-it-all-works)
- **FACT.** Поддержку вебмастеров ведёт персонаж «Платон Щукин». [yandex.ru/blog/platon](https://yandex.ru/blog/platon)
- **MEME.** Его ответы шаблонные, а «СДЛ» («сайты для людей») — жаргон SEO-сообщества. Пример обсуждения: [partnerkin](https://partnerkin.com/archives/7036); жалобы на «странные ответы от роботов» — в комментариях блога Платона.
- → в k8s:
  - Ранжирование endpoints в Service: `ya-proxy` выбирает бэкенд по YATI-скору.
  - Admission-плагины: `Minusinsk` понижает приоритет Pod'ов с «покупными» ownerReferences, `BadenBaden` режет Pod'ы с переоптимизированными labels (больше 64 штук), `AGS` выкидывает «дорвей»-сервисы без эндпоинтов.
  - Версии релизов названы городами по правилу последней буквы.
  - Все ошибки API-сервера подписаны «С уважением, Платон Щукин» и отвечают «развивайте ваш Pod для людей».

---

## 6. Реорганизации и корпоративная история (строго факты)

- **FACT.** 19 августа 2020 года Яндекс.Такси стал Яндекс Go — «суперприложением». Ещё в марте 2020-го в него добавили Еду и Лавку. Yango — сокращение от Yandex Go. [iPhones.ru](https://www.iphones.ru/iNotes/yandekstaksi-pereimenovali-v-yandeksgo-teper-eto-oficialno-superprilozhenie-08-19-2020), [Habr](https://habr.com/ru/post/515716)
- **FACT.** С 1 октября 2020 года «Беру» вошёл в Яндекс.Маркет. 15 декабря 2020 года Яндекс.Деньги стали ЮMoney (после того как Сбербанк выкупил долю). [iGuides](https://www.iguides.ru/main/other/marketpleys_beru_pereekhal_v_yandeks_market_chto_izmenilos_dlya_pokupateley/), [Rozetked](https://rozetked.me/news/13098-sberbank-pokazal-novyy-brend-dlya-yandeks-deneg-servis-budet-nazyvat-sya-yumoney)
- **FACT.** 22 августа 2022 года подписано, 12 сентября 2022-го закрыто: «Дзен» и «Новости» ушли к VK, Delivery Club — к Яндексу. Главной страницей стал ya.ru. [Новая газета Европа](https://novayagazeta.eu/articles/2022/09/12/iandeks-i-vk-zakryli-sdelku-po-obmenu-servisov-novosti-i-dzen-na-delivery-club-news), [dp.ru](https://www.dp.ru/a/2022/08/23/JAndeks_peredal_VK_Dzen)
- **FACT.** 5 февраля 2024 года Yandex N.V. объявила о продаже российского бизнеса за 475 млрд ₽ (дисконт не менее 50%) ЗПИФ «Консорциум.Первый», половина деньгами, половина акциями. Сделка закрыта в июле 2024 года. [The Moscow Times](https://ru.themoscowtimes.com/2024/02/05/yandex-nv-soglasovala-prodazhu-rossiyskikh-aktivov-konsortsiumu-investorov-za-475-mlrd-r-a120529), [Habr](https://habr.com/ru/news/813941)
- **FACT.** С 21 августа 2024 года Yandex N.V. называется Nebius Group (тикер NBIS), CEO — Аркадий Волож. [Nebius newsroom](https://nebius.com/newsroom/nebius-group-n-v-announces-official-name-change-and-new-ticker-symbol), [Wikipedia](https://en.wikipedia.org/wiki/Nebius_Group)
- **FACT.** Закрытия и заморозки: «Яндекс Кью» с 28 июня 2023 года работает в режиме чтения («музей контента»; Кью — бывший TheQuestion плюс «Знатоки»). В сентябре 2020-го Коллекции стали приватными, API закрыт. [devby](https://devby.io/news/yandeks-zamorozil-servis-voprosov-i-otvetov-yandeks-ku), [yandex.ru/dev/collections](https://yandex.ru/dev/collections/)
- → в k8s:
  - `kubectl rename` — ресурс, который меняет имя каждые два минорных релиза (`Taxi` → `Go`, `Beru` → `Market`, `Dengi` → `YuMoney`, kind переезжает в чужую API group).
  - Миграция namespace в другой кластер «с дисконтом не менее 50%» (`kubectl cluster split --discount=0.5`): кластер делится на две половины с разными API group.
  - Ресурсы в статусе `ReadOnly` (режим «музея», как у Кью).
  - Можно подать как *conversion webhooks*: ресурс переименовывается, а старые объекты висят в режиме только для чтения.

---

## 7. Пользовательские сервисы

- **FACT.** Алиса запущена 10 октября 2017 года. С первого дня у неё был более резкий тон, чем у конкурентов, и пользователи выводили её на провокационные ответы. Цитата сотрудника в заголовке «Медузы»: «Не то чтобы у нас была какая-то машинная комната, в которой мы можем регулятор хамства убавить».
  Источники: [Яндекс, «Представляем Алису»](https://yandex.ru/blog/company/alisa), [Meduza](https://meduza.io/feature/2017/10/20/ne-to-chtoby-u-nas-byla-kakaya-to-mashinnaya-komnata-v-kotoroy-my-mozhem-regulyator-hamstva-ubavit), [Meduza: «в соцсетях продолжают издеваться»](https://meduza.io/shapito/2017/10/11/alisa-za-mnoy-sledit-fsb-v-sotssetyah-prodolzhayut-izdevatsya-nad-golosovym-pomoschnikom-yandeksa)
  → в k8s: в kubectl есть параметр `--rudeness-level` (по умолчанию 7, «регулятора нет»); `kubectl explain` отвечает голосом Алисы.
- **FACT.** Капча-страница поиска: «запросы, поступившие с вашего IP-адреса, похожи на автоматические». Узнаваемый текст. [копия на gist](https://gist.github.com/anonymous/465f91f6147ac24ac0d5604142e30562)
  → в k8s: ответ 429 от API-сервера приходит с этим текстом и требует пройти SmartCaptcha («Я не робот») — даже контроллерам. ServiceAccount'ы должны поставить галочку.
- **FACT.** SmartCaptcha: обычно достаточно галочки «Я не робот», задание показывают только «подозрительным». В справке есть разделы «Часто появляется капча», «Не могу пройти». [справка](https://yandex.ru/support/smart-captcha/problems.html)
- **FACT.** Вебвизор в Метрике записывает действия посетителей, включая заполнение форм; поля можно исключать из записи. [справка Метрики](https://yandex.ru/support/metrica/webvisor/settings.html)
  → в k8s: `kubectl webvisor pod/x` воспроизводит запись всех `exec`-сессий. Это аудит-лог как видео.
- **FACT.** Лавка (пилот 2019 года, Хамовники) обещала доставку быстрее 15 минут; на тестах в среднем было 10 минут. [Habr](https://habr.com/ru/post/487870), [retail.ru](https://www.retail.ru/news/yandeks-zapustil-magazin-s-besplatnoy-dostavkoy-yandeks-lavka)
  → в k8s: SLA на запуск Pod — 15 минут. Образы лежат в «даркстор»-кэше на каждой ноде (pre-pull DaemonSet `lavka`).
- **FACT.** «Повышенный спрос»: ФАС описывает его как «повышающий коэффициент», множитель к базовой цене. Яндекс объясняет, что он привлекает водителей в район. **MEME / жалобы**: «включает чаще, чем требуется». [Habr, комментарии](https://habr.com/en/companies/yandex/articles/429226/comments), [zerkalo](https://news.zerkalo.io/life/28889.html)
  → в k8s: surge pricing для ResourceQuota — при нехватке нод квота на CPU умножается на коэффициент ×1.7; `kubectl get nodes` показывает «повышенный спрос».
- **FACT.** Пробки — шкала 0–10 баллов (0 — свободно, 10 — город «стоит»), балл отражает отклонение от нормы. [Яндекс, «Как работают Пробки»](https://yandex.ru/company/technologies/yaprobki), [kapital.kz](https://kapital.kz/gosudarstvo/66014/yandeks-nachal-ocenivat-zagruzhennost-ulic-almaty-i-astany.html)
  → в k8s: вместо Conditions у кластера — `Пробки: 7 баллов`; при 10 баллах scheduler отказывается работать, «город стоит».
- **FACT.** Нейро (апрель 2024) — ответ поиска, собранный YandexGPT из выдачи со ссылками; в октябре 2024-го интегрирован в Поиск. [ppc.world](https://ppc.world/news/yandeks-zapustil-servis-neyro/), [Rozetked](https://rozetked.me/news/33109-yandeks-predstavil-neyro-prodvinutyy-poisk-v-internete-pri-pomoschi-neyrosetey)
  → в k8s: `kubectl describe` сверху выдаёт «Нейро-ответ» с пересказом Events и ссылками на них.
- **FACT.** Яндекс.Бар, потом Элементы Яндекса ставились «в нагрузку» к чужим программам («сделать Яндекс стартовой страницей»). Пользователи жаловались на установку без явного согласия. [Habr, комментарии](https://habr.com/en/articles/158487/comments), [qna.habr](https://qna.habr.com/q/130301)
  → в k8s: Helm при установке любого чарта предлагает галочку «Установить Яндекс.Элементы в кластер», и по умолчанию она включена.

---

## 8. Внутренняя культура и инструменты (только то, что публично)

- **FACT.** Внутренние сервисы видны в публичном описании приложения «Яндекс Team» для сотрудников: профили Стаффа, календарь, карты офисов, командировки. Вход — только через аккаунты yandex-team.ru. [Trashbox](https://trashbox.ru/topics/189250/yandeks-team-2.40.1)
- **FACT.** «Этушка» — внутренняя соцсеть, упоминается в вакансии редактора внутреннего медиа. [quick-offer](https://quick-offer.ru/job/redaktor-vo-vnutrennee-media-71198)
- **FACT.** Яндекс тестировал среди сотрудников соцсеть «Аура» с лентой на ИИ. [Inc. Russia](https://incrussia.ru/news/yandeks-zapuskaet-sobstvennuyu-sotsialnuyu-set-aura/)
- **FACT.** Yandex Deploy — точка входа над YP (см. 1.4).
- → в k8s: RBAC берёт группы из CRD `StaffProfile` (`staff.yandex-team.ru/v1`); вместо Slack-нотификаций Events постятся в Этушку; `kubectl whoami` показывает карточку Стаффа.

---

## 9. Прочее, хорошо ложащееся на k8s

- **FACT.** Слоган «Найдётся всё». [ru.wikipedia: Яндекс](https://ru.wikipedia.org/wiki/%D0%AF%D0%BD%D0%B4%D0%B5%D0%BA%D1%81)
  → `kubectl get` никогда не возвращает NotFound — «найдётся всё», в худшем случае Нейро-ответ.
- **FACT.** Суперприложение: в Яндекс Go — Такси, Лавка, Еда, Драйв, Маркет. [iPhones.ru](https://www.iphones.ru/iNotes/yandekstaksi-pereimenovali-v-yandeksgo-teper-eto-oficialno-superprilozhenie-08-19-2020)
  → один бинарь `ya` вместо kubectl, helm, kustomize, crictl, etcdctl (как `ya make`, `ya tool`).
- **FACT.** Главная yandex.ru после сделки с VK стала dzen.ru, а Яндекс ушёл на ya.ru. [comnews](https://www.comnews.ru/content/221844/2022-08-24/2022-w34/yandexru-stanet-fantomnym)
  → API group `yandex.ru` помечена deprecated, всё переехало в `ya.ru`, старый адрес отдаёт редирект в чужой кластер.
- **FACT.** Масштаб NIH, оказавшийся мировым стандартом: ClickHouse, CatBoost.
  → честный раздел статьи: «иногда велосипед становится единорогом» — ClickHouse, Inc. стоит $2 млрд. [Inc. Russia](https://incrussia.ru/news/startap-clickhouse-edinorog/)

---

## Не подтверждено — не использовать как факт

- Жаргон «катить», «дежурство», «ABC» (сервис учёта ответственных за сервисы) — на слуху у бывших сотрудников, но публичного первоисточника в этом поиске не нашёл. Если нужно, ищите целево: «abc.yandex-team.ru», доклады Яндекса на HighLoad++ про on-call.
- Мемы про SmartCaptcha («бесконечная капча») и про «повышенный спрос» как отдельные мемы с первоисточником (Пикабу, Twitter) — не найдены. Есть только жалобы пользователей.
- Конкретный штраф ФАС или Роспотребнадзора именно за сложную отписку от Плюса — не найден. Есть только рекламное дело 2023 года.
- Закрытие Яндекс Мессенджера в 2023 году — не подтверждено. Подтверждено только слияние с Телемостом.

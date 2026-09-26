# GameCube SDK gap audit v2

Audit en cours, 2026-09-23. Base : `5da6b3fd37017705a1ca140329af0ccc3dc4fb13`, working tree initial propre.
Les patches incrémentaux et les journaux de validation sont conservés dans `.gekkoaot/audit-v2/`.
Aucun code ou objet propriétaire SDK n’est incorporé. Les références sont locales sous `debug/DolphinSDK1.0/`.

## Portée et niveaux de preuve

Native = comportement indiqué implémenté, pas certification du sous-système entier. Partial = couverture partielle ou non validée. Missing = comportement absent constaté. Les lignes « inventory/pending » ne constituent pas une comparaison SDK terminée.
Les symptômes fournis par l’utilisateur ne sont pas des reproductions de cette session.

## Inventaire

- SDK : `debug/DolphinSDK1.0.zip` et arborescence extraite.
- PDF : `debug/DolphinSDK1.0/Docs/PDF.zip`, guides Architecture/Graphics/Audio/Programmer et `Docs/IBM/GekkoUserManual.pdf`.
- HPPOA : `debug/discs/Harry-Potter-and-the-Prisoner-of-Azkaban-USA/sys/main.dol`.
- Patch expérimental : `/home/linux/Téléchargements/GekkoAOT-v89-Aurora-segmented-storage-from-v88.patch`.
- L’arbre actuel contient déjà `gekkoaot-storage-staging-v4.patch` et son application dans `gekkoaotctl.cpp` : ne pas appliquer aveuglément v89.

## HPPOA : preuve et cause

`80008ab0: 7c79eaa6 mfspr r3,953`; `80008ab4: blr`.
La boucle `8004f224..8004f234` appelle ce lecteur, soustrait la lecture initiale et attend de dépasser un seuil signé.
Les appels en `800517f4` et `802508bc` écrivent MMCR0 via `80008aa0` avec `0x4b` : PMC1SELECT=1 (cycles processeur), PMC2SELECT=11.
Le SPR 953 est PMC1 (manuel Gekko, chapitre 11). Le runtime initial conserve sa valeur dans `spr_state_` mais ne l’incrémente pas. C’est une cause suffisante de non-progression de cette attente lorsque son seuil est positif. Ce n’est pas une preuve de panne DEC.

DolRecomp dispose déjà de budgets de boucle et de retours au dispatcher. Une lecture de compteur ne doit pas inventer des cycles, ni doubler ceux comptabilisés par `AdvanceRuntimeCycles`. Les reads TB directs du backend et la visibilité à l’intérieur d’une slice demandent une validation distincte.

## VM : contrat établi

Les manuels `VMInit`, `VMBASEInit`, `VMBASEGetPhysicalAddrInMRAM` et `vmbase.h` décrivent des pages de 4096 octets, un pool pris dans l’arène MEM1, un backing ARAM fourni par l’application et des gestionnaires DSI/ISI. La table de pages, les pages verrouillées et la table inverse sont distinctes.
Le tableau historique `fake_vmem_` n’est ni un alias des pages MEM1 ni un backing ARAM ; il évite aussi les défauts de pages. Le bloc v95 utilise la table invitée pour les accès CPU à la fenêtre SDK si un SDR1 non nul est installé et si tous les modules actifs peuvent reprendre un accès mémoire AOT interrompu. Le bloc v97 conserve le tableau historique pour les anciens modules compilés sans cette capacité. La plage est confirmée par `man/vm/intro.html` : [0x7E000000, 0x80000000). La conformité complète de la pagination n’est pas établie. MEM1 reste 24 MiB. HPSS installe réellement SDR1 dans le smoke v97.

## Matrice

Le CSV associé est la version exploitable automatiquement.

### CPU PMC

- **behavior** : PMC1 cycle event; MMCR freeze; user aliases
- **reference** : GekkoUserManual chapter 11 tables 11-2 and 11-5..8
- **status** : Partial
- **implementation** : runtime/native/runtime.cpp:SprRead; AdvanceCpuTimers
- **impact** : Polling never terminates
- **observed_games** : HPPOA: DOL disassembly
- **priority** : P0
- **proposed_fix** : Implement cycle event respecting selectors and freeze
- **required_test** : PMC cycle/freeze/alias/wrap polling

### CPU DEC/TB

- **behavior** : CPU/12; DEC wrap; EE pending; RFI
- **reference** : GekkoUserManual chapter 2; DolRecomp timing.cpp
- **status** : Partial
- **implementation** : runtime/native/runtime.cpp:AdvanceCpuTimers; TryTakeDecrementerInterrupt
- **impact** : Values visible only at host boundaries
- **observed_games** : Not established for reported HPPOA loop
- **priority** : P1
- **proposed_fix** : Verify bounded AOT yields before changing timing ownership
- **required_test** : DEC wrap; EE off/on; RFI; TB remainder; compiled loops

### CPU exceptions

- **behavior** : DSI ISI alignment program syscall MSR SPR cache
- **reference** : GekkoUserManual; low-memory executor inspection
- **status** : Partial
- **implementation** : runtime/native/runtime.cpp; .gekkoaot/src/DolRecomp/src/cpu/cpu.c
- **impact** : Missing MMU/page-fault semantics
- **observed_games** : Unverified
- **priority** : P1
- **proposed_fix** : Inventory exception paths and preserve ABI
- **required_test** : Exception entry/return vectors

### VM

- **behavior** : 4 KiB pages; MEM1 arena; ARAM backing; DSI/ISI
- **reference** : man/vm/vm/VMInit.html; VMBASEInit.html; vmbase.h
- **status** : Partial
- **implementation** : runtime/native/page_table.h; runtime.cpp:AccessPagedVmem
- **impact** : FakeVMEM is independent storage not physical aliases
- **observed_games** : No usage proven from game identities
- **priority** : P1
- **proposed_fix** : Introduce translations only after page-table/CPU/GX contract is established
- **required_test** : VM mapping eviction dirty pages ARAM aliases

### MEM1

- **behavior** : 24 MiB with cached/uncached aliases
- **reference** : runtime address-space inspection
- **status** : Native
- **implementation** : runtime/native/address_space.h
- **impact** : Invalid pointers must remain invalid
- **observed_games** : All
- **priority** : P1
- **proposed_fix** : Retain exact bounds
- **required_test** : Boundary crossing and mirrors

### Aurora storage

- **behavior** : 64 MiB GPU buffer; segmented staging; pass rollover
- **reference** : Current staging-v4.patch and materialized Aurora
- **status** : Partial
- **implementation** : patches/aurora/gekkoaot-storage-staging-v4.patch
- **impact** : Cached ranges and pass ordering on rollover
- **observed_games** : HPCOS overflow reported by user; not reproduced here
- **priority** : P0
- **proposed_fix** : Audit existing v4 instead of reapplying stale v89
- **required_test** : Forced rollover; fog/arrays; GPU ordering

### CP/FIFO/WGPIPE

- **behavior** : 32-byte bursts; breakpoint; IRQ; watermarks; wrap
- **reference** : Current native_cp; SDK demos mgt-fifo-brkpt/mgt-fifo-dual inventoried
- **status** : Partial
- **implementation** : runtime/hw/cp/native_cp.cpp; runtime/native/runtime.cpp
- **impact** : Stalled producer/consumer
- **observed_games** : HPPOA earlier breakpoint reported
- **priority** : P1
- **proposed_fix** : Exercise current v88 and FIFO command visibility
- **required_test** : CP FIFO breakpoint/wrap/watermarks

### GX/XF/cache

- **behavior** : Indexed XF load lifetime; vertex/texture invalidation
- **reference** : Current bridge; SDK demo families inventoried, not fully compared
- **status** : Partial
- **implementation** : runtime/gx/aurora/bridge.cpp
- **impact** : Geometry/texture corruption
- **observed_games** : Multi-game symptoms reported, causes unproven
- **priority** : P1
- **proposed_fix** : Trace exact invalidation and memory generation transitions
- **required_test** : Indexed loads; vertex cache; texture/TLUT bounds

### OS

- **behavior** : Context scheduler queues alarms mutex semaphores
- **reference** : native_os.h present; SDK OS API inventory pending
- **status** : Partial
- **implementation** : runtime/os/native_os.h
- **impact** : Missed wakeups or context corruption
- **observed_games** : Animaniacs reported freeze
- **priority** : P1
- **proposed_fix** : Identify expected wake event from new trace
- **required_test** : Context save/load; queues; alarms; priority

### Multi-image

- **behavior** : Overlapping DOL/ELF REL routing; icbi invalidation
- **reference** : runtime dispatch and fixed-image routing inspected
- **status** : Partial
- **implementation** : runtime/native/runtime.cpp; tools/gekkoaotctl.cpp
- **impact** : Wrong code selected after replacement
- **observed_games** : MOH reported stuck
- **priority** : P1
- **proposed_fix** : Trace active identity and ambiguous identical chunks
- **required_test** : Overlapping images and cross-image indirect returns

### PI

- **behavior** : Interrupt cause/mask and FIFO CPU pointers
- **reference** : native_pi.h/cpp; SDK comparison pending
- **status** : Partial
- **implementation** : runtime/hw/pi/native_pi.cpp
- **impact** : IRQ and producer visibility
- **observed_games** : Unverified
- **priority** : P1
- **proposed_fix** : Register read/write/ack tests
- **required_test** : PI masks level delivery FIFO wrap

### MI

- **behavior** : Protection regions IRQ and timers
- **reference** : native_mi.h explicitly documents missing enforcement
- **status** : Partial
- **implementation** : runtime/hw/mi/native_mi.cpp
- **impact** : Protection/timer reads approximate
- **observed_games** : Unverified
- **priority** : P2
- **proposed_fix** : Add enforcement only with hardware scenarios
- **required_test** : MI protection and interrupt ack

### PE

- **behavior** : Token/finish latches enable ack bbox
- **reference** : native_pe.h/cpp; SDK GX comparison pending
- **status** : Partial
- **implementation** : runtime/hw/pe/native_pe.cpp
- **impact** : Completion ordering and bbox accuracy
- **observed_games** : Unverified
- **priority** : P1
- **proposed_fix** : Test FIFO event propagation and ACK
- **required_test** : PE token/finish

### VI/XFB

- **behavior** : Beam fields interrupts per-XFB history
- **reference** : native_vi.h; current bridge; v82 regression requirement
- **status** : Partial
- **implementation** : runtime/hw/vi/native_vi.cpp; runtime/gx/aurora/bridge.cpp
- **impact** : Scanout glitches
- **observed_games** : HPCOS improvement reported
- **priority** : P1
- **proposed_fix** : Preserve history; test field timing
- **required_test** : VI retrace and alternating XFB

### DSP/AX

- **behavior** : Mailboxes tasks voices AFC PCM SRC aux
- **reference** : Native DSP/HLE source inventory; SDK comparison pending
- **status** : Partial
- **implementation** : runtime/dsp/native/native_dsp.cpp; native_ax_hle.cpp; native_jaudio_hle.cpp
- **impact** : Audio or callback stalls
- **observed_games** : Unverified
- **priority** : P2
- **proposed_fix** : Build task/voice conformance scenarios
- **required_test** : DSP mail task switching voice lifecycle

### DSP LLE

- **behavior** : Unknown microcode fallback
- **reference** : runtime/native/CMakeLists.txt
- **status** : Partial
- **implementation** : runtime/dsp/native/native_dsp_core.cpp
- **impact** : Uses GPL Dolphin instruction semantics; no Core link
- **observed_games** : Unverified
- **priority** : P2
- **proposed_fix** : Document fallback coverage separately
- **required_test** : Unknown task bootstrap

### AI

- **behavior** : DMA sample counter streaming IRQ
- **reference** : Native AI implementation inventory; SDK comparison pending
- **status** : Partial
- **implementation** : runtime/hw/ai/native_ai.cpp
- **impact** : Audio callbacks
- **observed_games** : Unverified
- **priority** : P2
- **proposed_fix** : Check rates and DMA cadence
- **required_test** : AI sample rollover and DMA callback

### ARAM

- **behavior** : AR DMA ARQ callbacks and allocation
- **reference** : SDK VM documentation; DSP source inventory
- **status** : Partial
- **implementation** : runtime/dsp/native/native_dsp.cpp
- **impact** : VM backing coherence / callback timing
- **observed_games** : Unverified
- **priority** : P1
- **proposed_fix** : Verify shared ARAM source of truth
- **required_test** : ARAM DMA and VM pages

### DI

- **behavior** : DMA async completion cancel status streaming
- **reference** : AdvanceRuntimeCycles DI completion inspected
- **status** : Partial
- **implementation** : runtime/hw/di/native_di.cpp; runtime/native/disc_reader.cpp
- **impact** : Missed DVD completion
- **observed_games** : Unverified
- **priority** : P2
- **proposed_fix** : Keep nod backend; test cancel/status races
- **required_test** : DI async complete/cancel/error

### EXI

- **behavior** : Card SRAM RTC select DMA IRQ
- **reference** : Source inventory only; SDK comparison pending
- **status** : Partial
- **implementation** : runtime/hw/exi/native_exi.cpp
- **impact** : Peripheral compatibility
- **observed_games** : Unverified
- **priority** : P2
- **proposed_fix** : Audit command/IRQ timing
- **required_test** : EXI transfer/card/RTC

### SI

- **behavior** : Four ports polling PAD rumble IRQ
- **reference** : Source inventory only; SDK comparison pending
- **status** : Partial
- **implementation** : runtime/hw/si/native_si.cpp
- **impact** : Input polling/wakeup
- **observed_games** : Unverified
- **priority** : P2
- **proposed_fix** : Validate transfer/status/interrupt behavior
- **required_test** : SI four-port poll and ACK


## Validation du bloc PMC / timers

- Build `./build-linux.sh` : réussi.
- CTest : DEC, TB, EE, RFI, PMC cycle/hold/freeze/alias/wrap et TBL edges.
- Pytest : trois DOL PowerPC créés indépendamment, compilés LLVM et exécutés par le vrai runtime. PMC1, DEC et TB sortent des boucles via les budgets AOT. Deux horizons sont testés (0 et 262144).
- Les lectures ne créent aucun tick ; le domaine matériel existant reste l’unique source de temps. Les registres restent figés dans une slice : précision instruction par instruction non garantie.
- PMC événements instructions/cache/stalls et IRQ 0xF00 non implémentés. PMCTRIGGER est échantillonné à la frontière matérielle, sans découpage exact d’une tranche traversant le seuil PMC1. Les changements MSR/MMCR au milieu d’une tranche ne disposent pas encore d’un historique par instruction.
- Retest HPPOA complet : en cours, aucune promesse de disparition de tous les blocages.

## Storage v89/v4 : état réellement constaté

`git -C .gekkoaot/src/Aurora apply --reverse --check patches/aurora/gekkoaot-storage-staging-v4.patch` (avec chemin absolu du patch) réussit : les changements sont déjà matérialisés.
Le préflight se fait avant les uploads du draw ; le rollover scelle la passe et en reprend une en chargement, invalide les arrays ainsi que les plages fog/uniform. Les high-water contiennent le segment ; l’encodeur copie le nouveau segment avant la passe correspondante. Les buffers overflow restent vivants jusqu’à la soumission.
L’ancien patch v89 n’a pas à être réappliqué ni son ancien hunk de `gekkoaotctl.cpp` forcé. Le test GPU indépendant `test_storage_rollover.py` force deux rollovers à 64 MiB sur deux frames avec arrays indexés et fog : il passe sur Vulkan Intel LNL. Il vérifie les erreurs et les frontières de segments, pas la justesse des pixels. Le retest HPCOS reste nécessaire.

## Registres PI / MI / PE / VI

Inventaire par adresse dans `GameCube-register-audit-v2.csv` : lectures, écritures, acquittement, IRQ et fonction. Ce tableau décrit le code actuel, pas une certification matérielle.

Lacunes établies : les dix compteurs MI ne progressent pas ; la protection MI n’est pas appliquée ; les BBox PE ne reçoivent pas de mises à jour du renderer ; les compteurs PE renvoient zéro. Les ACK PE sont modélisés ; les IRQ VI ont des latches/masques et une position de faisceau calculée.

## CP : correction indépendante v92

Le scénario indépendant de 128 octets a échoué avant correction : `FifoReadWriteDistance() == 128` était faux (100). Les écritures CP masquent les cinq bits bas de top, ce qui donne l’adresse du dernier burst et impose un `+32` dans le calcul de capacité. Le `+4` de v88 réutilisait la convention de la structure SDK après que l’alignement du registre l’avait déjà transformée.
Le test vérifie aussi un gather partiel, l’arrêt au breakpoint avec IRQ, sa désactivation et le wrap.

## Audio / périphériques : état de l’inspection

| Comportement | État | Preuve / limite |
|---|---|---|
| DSP mailboxes / bootstrap / tâches | 🟠 Partial | `native_dsp.cpp` modélise ownership et séquences ; couverture des tâches non certifiée |
| AX PB / PCM / AFC / SRC / mixer | 🟠 Partial | `native_ax_hle.cpp` ; familles/formats à tester contre scénarios indépendants |
| AUX / DPL2 | 🟠 Partial | `MixAux` et branches `dpl2` présentes ; pas de validation audio de référence |
| Early JAudio | 🟠 Partial | VPB, décodage ARAM et sources non prises en charge signalées de manière bornée |
| LLE | 🟠 Partial | Fallback conditionnel disponible ; sources LLE non matérialisées dans ce build, donc backend désactivé ici |
| Sortie audio hôte | ⚪ Host abstraction | `native_audio_sink.cpp` |
| AI DMA / streaming / compteurs | 🟠 Partial | `native_ai.cpp` ; callbacks/fréquences à valider |
| ARAM / ARQ | 🟠 Partial | DMA dans DSP ; pagination VM et cohérence des pages non intégrées |
| DI asynchrone | 🟠 Partial | complétion différée/cancel dans `AdvanceRuntimeCycles` ; nod conservé |
| EXI card / SRAM / RTC | 🟠 Partial | implémentations présentes ; timing/commandes à tester |
| SI / PAD | 🟠 Partial | quatre canaux et polling présents ; états input/rumble à tester |

## GX / OS / multi-image : invariants à poursuivre

- Le bridge maintient les fenêtres XF exactes, l’invalidation texture BP 0x66 et les générations texture/TLUT. Les seules allocations storage Aurora trouvées concernent arrays et fog LUT, couvertes par le préflight v4.
- Le CP reste synchrone ; le bloc v94 impose maintenant GPRead et les breakpoints avant la consommation NativeGX. Le test indépendant vérifie les octets effectivement livrés au backend.
- OS : queues messages/mutex, transferts de mutex, réconciliation READY et alarmes existent. Aucun événement manquant n’est prouvé pour Animaniacs sans nouveau dump/trace de sa queue d’attente.
- MOH : sélection par identité de chunk et voisinage, persistance de l’image active, routing indirect global et invalidation icbi existent. Aucun nouveau retest MOH ne permet encore de conclure sur son blocage.


## WGPIPE / consommation CP : bloc v94

- **Preuve** : Gekko User Manual, §9.4.2, décrit des transferts de lignes complètes de 32 octets. `sync`, `eieio` et `stwcx.` ne vident pas le write gather ; écrire WPAR abandonne les données en attente. Son bit BNE indique la présence de données.
- **Cause** : le runtime publiait les octets au backend avant de tester GPRead/breakpoint, et son raccourci de vecteur PPCSync vidait artificiellement les gathers partiels.
- **Fix** : un burst complet est écrit au FIFO PI en MEM1, puis consommé par un callback CP seulement si la lecture est autorisée. Un FIFO GP non lié peut également être consommé depuis la RAM. Suppression du flush artificiel et abandon des données lors d’une écriture WPAR. Le calcul initial de BNE à partir de tout gather partiel a été corrigé au bloc v105.
- **Validation** : `test_wgpipe.cpp` échoue avant correction sur la consommation alors que GPRead est arrêté. Après correction, il vérifie arrêt/reprise/breakpoint, ordre des octets, visibilité RAM, conservation du partiel à PPCSync, abandon WPAR et FIFO non lié avec wrap. Backend GX factice indépendant, sans code SDK.
- **Limites** : modèle synchrone, sans FIFO matériel interne de 128 octets ni backpressure cycle-exact. Le gating HID2[WPE] et le comparateur d’adresse WPAR ne sont pas encore complets. Le chemin historique sans FIFO PI configuré reste disponible. Pas de retest jeu complet attribuable à ce bloc à ce stade.


## VM / reprise des exceptions AOT : bloc v95

**Références** : manuel Gekko §4.5.3, §5.4, table 5-8 ; PowerPC Programming Environments §7.5–7.6 ; manuels VMBASEInit et VM du SDK. Implémentation indépendante, sans code ou binaire SDK incorporé.

**Fix** : lorsque le programme installe un SDR1 non nul, les accès CPU dans [0x7e000000, 0x80000000) suivent les DBAT puis la table de pages invitée. Recherche bornée à deux groupes de huit PTE, VSID/API/H, clés Ks/Kp et PP, bits R/C, frames physiques MEM1 uniquement. Une page absente ou protégée entre en DSI avec DAR/DSISR/SRR0/SRR1 ; un accès chevauchant deux pages respecte leurs frames indépendantes. Les accès GPU et les pointeurs HLE ne voient plus le backing fictif dans ce mode. Le GPU ne sait pas traduire les adresses virtuelles SDK : le manuel VM exige des adresses fixes pour GPU/DSP.

**Bugs reproduits** : le DOL indépendant répare sa PTE dans le vecteur DSI puis exécute RFI. Avant le patch compilateur, sa reprise sur un `lwz` intérieur passe au fallback car le bloc LLVM n’a pas d’entrée à cette adresse. Le fallback signalait une erreur hôte sans sortir du corps AOT, produisant une boucle infinie. Le patch DolRecomp v14 ajoute les points de reprise mémoire et invalide son cache d’objets ; le cache de modules GekkoAOT change également. Une erreur fatale hôte positionne maintenant un latch de sortie distinct des exceptions invitées.

Un second test a reproduit l’écrasement du PC d’exception et des FPR par le fallback de chargement flottant. Ce chemin vérifie maintenant le latch d’exception avant de modifier destination/base/PC.

**Validation** : tests C++ de protections, hash primaire/secondaire, R/C, bornes, DBAT, accès interpage, défaut DSI et RFI ; DOL LLVM indépendant passant réellement par son gestionnaire invité et retentant le load à deux budgets AOT. Les tests matériels préexistants passent.

**Encore manquant** : MMU générale hors fenêtre SDK ; fetch ISI et identité du code paginé ; accès HLE par spans paginés ; TLB/cache précis ; table à base physique zéro en mode migration ; lifecycle VM/éviction/ARAM validé sur SDK et jeux. Une table ou frame hors MEM1 produit une erreur hôte explicite, pas une émulation complète du bus/machine-check. Le parcours ne crée pas une deuxième ARAM. Aucun jeu n’est déclaré corrigé par ces seuls tests. Les nouveaux points de reprise peuvent réduire la fusion LLVM des régions ; aucun gain de performance n’est revendiqué.


## Diagnostics / smoke tests multi-jeux : bloc v96

Le runner affiche désormais le résultat et un snapshot borné avant sa sortie PGO sans teardown. Avant correction, un stop correctement acquitté quittait le processus avant le résultat final. Le snapshot de fin contient GPR/CR/PC/LR, CP/FIFO, VI/XFB, thread/contexte courant et jusqu’à 16 threads actifs avec queue/mutex. Les adresses de queue ne sont pas présentées comme une preuve de source de réveil.

Les premiers résultats sont dans `GameCube-multigame-smoke-v1.csv`. HPCOS : init OS/GX, lectures DI, bootstrap DSP et génération audio observés ; aucune nouvelle frame GX pendant l’observation, présentations XFB seules insuffisantes pour valider le boot. HPSS : usage VM réel observé sur 0x7e000000, défaut DSI et retour invité ; ancien module compilé sans points de reprise mémoire, échec sur stw 0x8017ed94. Ce résultat impose une migration selon capacité du module, avant de qualifier la VM utilisable avec les anciens caches.

Validation complémentaire DolRecomp : tests de sémantique et trois ABI d’exécution passent. Le test `llvm_ir` échoue sur l’attente d’optimisation paired-single de func_800039C0_budget. Le même échec est reproduit dans un binaire de contrôle reconstruit avec register_state.cpp antérieur au v14 (journal `.gekkoaot/audit-v2/llvm-control.log`) ; il est préexistant à cette correction, mais reste non résolu.

## Migration des modules, HPSS/HPCOS : bloc v97

Le smoke v96 a exposé un défaut de migration : HPSS installe SDR1, mais son module LLVM en cache ne possède pas les nouvelles entrées AOT au milieu des accès mémoire. Une DSI invitée suivie d’un RFI revenait sur un `stw` intérieur impossible à reprendre et déclenchait le fallback hôte. Le compilateur v15 signale maintenant la capacité de reprise dans l'en-tête généré ; le module l'exporte par un symbole optionnel sans changer l'ABI v3. Le runtime n'active les pages réelles que si le module principal et les modules secondaires actifs possèdent cette capacité. Il refuse de charger un ancien module secondaire une fois la pagination active.

Le DOL de test recompilé exporte la capacité, active la pagination, répare sa PTE en DSI et reprend après RFI aux deux budgets AOT. Le module HPSS déjà en cache n'exporte pas la capacité. Au smoke v97, HPSS choisit donc explicitement le chemin historique (`paging=0`), tourne 30 secondes sans l'échec v96, effectue 375 lectures DI sans erreur, émet des commandes GX avec copies EFB répétées et initialise DSP/audio. HPCOS tourne aussi 30 secondes sans erreur fatale. Ces traces ne prouvent ni des pixels corrects, ni le menu, ni le gameplay ; HPCOS n'a toujours aucune nouvelle frame GX mesurée. Les résultats et PC finaux figurent dans le CSV smoke.

## VI, AI et DI : blocs v99, v98 et v100

- VI : les comparateurs HCT/VCT étaient évalués uniquement au bord des demi-lignes, de sorte que HCT=25 n'interrompait jamais au cycle attendu et HCT=125 pouvait interrompre trop tôt. Le test indépendant reproduit ces erreurs, puis valide le cycle, l'acquittement, le réarmement et le champ après correction. L'histoire XFB et les pixels présentés restent non vérifiés en jeu.
- AI : un trigger d'interruption nul se déclenchait au wrap du compteur ; le compteur ralentissait à 32 kHz lorsque le flux d'entrée utilisait ce mode, alors qu'il est après conversion. Le test échoue avant et passe après correction. La qualité sonore et le pitch en jeu ne sont pas déduits de ces registres.
- DI : un fournisseur de média monté répondait malgré un couvercle ouvert ; une ancienne complétion asynchrone pouvait finaliser une nouvelle requête après BREAK ou reset. Le test indépendant vérifie absent-media, requêtes séquencées, annulation, ACK et reset. Le runtime rejette aussi les chevauchements avant toute écriture DMA et ignore les complétions devenues obsolètes. Le backend de lecture des formats disque reste nod/Aurora. Les courses réelles avec thread hôte et le streaming DVD ne sont pas couverts.

## EXI : acquittement par voie écrite

Les causes TCINT et EXTINT de STATUS sont W1C. Une écriture 8/16 bits dans une autre voie réécrivait auparavant le mot entier lu et acquittait par erreur des causes hors de cette voie. Le test indépendant échoue avant correction sur TCINT perdu, puis couvre aussi EXTINT et l'ACK explicite après la correction. Référence comportementale : `include/dolphin/os/OSInterrupt.h` et désassemblage de la routine d'acquittement SDK locale, consultés sans reprise de code. Carte mémoire, RTC et timing des transferts restent non vérifiés.

## OS : priorité des attentes de messages

La documentation SDK `OSSendMessage` et `OSReceiveMessage` décrit le réveil du premier thread en attente selon sa priorité. La couche OS plaçait déjà les threads dans une queue ordonnée par priorité, mais les chemins de livraison d'un message et de libération d'une place parcouraient une table de hachage des attentes. Les deux scénarios sont reproduits avec des structures invitées et les vrais appels HLE de création, reprise, suspension et envoi/réception : avant correction, le receveur prioritaire ne reçoit pas le message et l'émetteur prioritaire ne remplit pas la place libre. Après correction, les deux chemins suivent la queue ordonnée et le test passe. Cela ne qualifie pas les alarmes, mutex ni un éventuel blocage Animaniacs, qui n'a pas été reproduit dans cette passe.

## GX XF : matrice indexée avec stride nul

`GXSetArray` définit l'adresse source comme base + indice × stride ; stride est un octet sans interdiction de zéro. Le pont natif calculait déjà correctement une fenêtre de 48 octets pour `GXLoadPosMtxIndx` avec stride nul, mais Aurora rejetait ensuite systématiquement le chargement. Le test FIFO autonome échoue avant correction sur les indices 7 et 65535, puis passe avec un seul objet de matrice réutilisé. Un second test confirme qu'une source tronquée à 47 octets reste rejetée. Les 206 tests FIFO Aurora passent après le correctif. La référence est `debug/DolphinSDK1.0/man/gfx/gx/Geometry/GXSetArray.html`. Aucun jeu ni pixel n'a encore été retesté pour ce cas, et la cohérence du cache vertex reste à auditer.

## WPAR et basculement FIFO : bloc v105

HPCOS bloquait à `0x8019922c–0x80199234` sur le test de WPAR[BNE]. La trace montrait 31 octets résiduels dans le write-gather, BNE=1, 43 lectures DI puis aucune nouvelle copie XFB. Le manuel Gekko §9.4 décrit des bursts de 32 octets et l'abandon du tampon par écriture WPAR ; la documentation SDK `GXFlush`, `GXResetWriteGatherPipe`, `GXBeginDisplayList` et `GXEndDisplayList` confirme que les zéros de bourrage peuvent rester au changement de FIFO. Le manuel ne détaille pas la visibilité exacte de BNE pour ce résidu ; l'interprétation du bit dans notre modèle synchrone repose aussi sur la boucle SDK observée dans ce DOL.

Une fois cette attente débloquée, HPCOS révélait une seconde faute hôte à `0x801991ec` : le runtime refusait de revenir au FIFO GPU avec sept zéros de bourrage en attente. Le test indépendant reproduit cette faute avant correction. Les deux chemins de capture partagent désormais les octets du même tampon physique de 32 octets ; si le FIFO PI change sans remise à zéro WPAR, le prochain burst complet va au FIFO alors sélectionné. Une écriture WPAR abandonne toujours les octets restants. Dans le modèle actuel, les bursts complets sont synchrones, donc BNE ne signale aucune transaction bus encore en attente lorsqu'une lecture invitée reprend.

Après correction, le test couvre le basculement FIFO temporaire vers GPU et l'inverse avec conservation et ordre des octets. Les 10 tests CTest passent. Dans un smoke HPCOS borné à 8 s : 333 lectures DI sans erreur, copies XFB répétées, scanout VI lié au cache XFB et 431 présentations hôte, sans faute fatale. Le smoke HPSS de 8 s reste sans faute, avec 39 lectures DI et des copies XFB répétées. Les journaux sont `.gekkoaot/audit-v2/hpcos-wpar-transfer-smoke/run.log` et `.gekkoaot/audit-v2/hpss-wpar-transfer-smoke/run.log`. Aucun de ces compteurs ne valide les pixels, le menu ou le gameplay. La file matérielle de 128 octets et sa contre-pression temporelle ne sont pas émulées cycle par cycle.

## Priorité Super Mario Sunshine et Mario Kart Double Dash

Les deux images RVZ ont été extraites par `nod` et leurs DOL reconnus : Sunshine `GMSE01`, entrée `0x8000522c` ; Double Dash `GM4E01`, entrée `0x80003154`. La recompilation AOT de Sunshine reste en cours et aucun module complet n'était dans le cache GUI lors de cette passe ; son exécution demeure non vérifiée. Un module Double Dash complet déjà présent dans le cache GUI a passé la vérification d'ABI et permis un smoke direct. Le logo Dolby apparaît vers 5 s et un écran coloré de sélection de circuit vers 20 s ; l'image blanche vers 15 s appartient à la transition et ne prouve pas un défaut d'exposition.

Double Dash s'arrêtait ensuite à `0x80206db0`, une instruction `blr` visée par une table de branchement indirecte à l'intérieur d'un chunk AOT sans étiquette de bloc compilée à cette adresse. Le fallback hôte ne savait pas exécuter `bclr`/`bcctr` et posait une erreur fatale. Le runtime exécute désormais ces deux branchements avec les tests CR/CTR, le lien et l'alignement de cible PowerPC. Le test `hardware.aot-indirect-branch` échouait avant correction puis passe ; les 12 tests CTest racine passent. Au premier retest, le jeu a dépassé ce PC et atteint 1 235 présentations, puis a révélé un shader WGSL invalide : une comparaison alpha TEV GR16 composait `0.0.rg`. Le patch Aurora `gekkoaot-tev-alpha-compare-v2.patch` reprend les opérandes couleur A/B avant l'écriture du registre destination pour ces modes. Son test shader GR16/A8 passe avec les trois autres tests de génération.

Avec ce patch, un smoke Double Dash de 45 s se termine par un arrêt volontaire sans faute CPU/GPU ni erreur DI : 608 lectures DI, environ 2 638 présentations, vitesse nominale rapportée à 100 %. Les captures montrent l'écran de sélection Grand Prix à 25–30 s puis la course à 45 s (`.gekkoaot/audit-v2/mario/mkdd-tev-v2-smoke-retry/`). La 3D de la course reste gravement incorrecte : ciel et HUD visibles, piste et décor noirs/absents. Ce résultat valide le franchissement des deux arrêts précédents, pas le rendu ni le gameplay. Le chemin de copie BP/EFB et les états de texture/profondeur au début de la course demandent une trace ciblée.

## SI : sélection des ports de polling : bloc v106

Le registre `SIPOLL` contient quatre bits d'activation de polling automatique, un par port. Le runtime appelait le hook de polling pour tous les ports à chaque échéance, y compris quand tous les bits étaient désactivés au reset. Les ports absents pouvaient ainsi poser de faux états sans réponse et réveils SI. Le test indépendant échoue avant correction dès le reset, puis vérifie l'activation des ports 0 et 2, l'absence de sondage des ports désactivés, l'interruption RDST et le transfert explicite COMCSR vers un port sans polling. Après correction, seul le hook automatique est filtré ; le transfert explicite reste disponible.

Références comportementales locales : `debug/DolphinSDK1.0/include/dolphin/os/OSSerial.h`, `man/prog/os/Serial/SISetSamplingRate.html` et les symboles `SIEnablePolling`/`SIDisablePolling` des MAP SDK. Les 11 tests CTest passent. Les smokes HPSS/HPCOS v106, huit secondes chacun avec ce changement, se terminent sans faute ; HPCOS effectue 333 lectures DI et 432 présentations hôte, HPSS 39 lectures DI et 439 présentations. Ce sont des vérifications de non-régression du démarrage, pas des validations d'entrée manette ni de pixels en jeu.

## Aurora : durée de vie des bind groups pendant les frames en attente

Le correctif EFB/XFB v110 de l'utilisateur est présent dans le working tree et a été conservé. En le retestant sur HPSS, le render worker Aurora a reçu `SIGSEGV` dans `find_bind_group()` vers 32 s, alors que le thread CPU soumettait les copies GX. Le cache évacuait ses ressources d'après `current_frame()` (le compteur du producteur CPU), qui peut être en avance sur la frame effectivement soumise par le render worker. En build release, la vérification `CHECK` de l'absence d'une entrée était supprimée, puis le code déréférençait l'itérateur de fin.

Le test `BindGroupCacheLifetime.RetainsReferencesFromQueuedFrames` échoue avant correction lorsque la dernière utilisation est dans une frame enregistrée mais encore en attente. L'expiration utilise désormais l'index de la frame soumise, avec une comparaison sûre lors du retour à zéro du compteur. Une première reprise a ensuite exposé un autre cas de même durée de vie : l'activation de la fenêtre HPSS à 35 s a redimensionné la surface de 1280×720 à 3072×1772 ; `clear_caches()` vidait tous les bind groups pendant qu'une frame ouverte en référençait encore un. Le cache offscreen est toujours remis à zéro au redimensionnement, mais les bind groups restent vivants jusqu'à leur expiration après soumission. Une référence introuvable produit maintenant une erreur fatale explicite en release plutôt qu'un `SIGSEGV` opaque.

Après les deux changements, un smoke HPSS de 52 s avec entrée clavier et redimensionnement s'arrête volontairement sans faute, avec 867 lectures DI, des présentations XFB répétées et des captures de cinématique 3D aux secondes 35 et 52 (`.gekkoaot/audit-v2/hpss-v112-resize-retain/`). Le même scénario HPCOS atteint 52 s sans faute, 1662 lectures DI, et montre la scène 3D du Terrier aux secondes 35 et 52 (`.gekkoaot/audit-v2/hpcos-v112-resize-retain/`). Ces captures montrent des pixels réels et de la géométrie dans ces scènes seulement ; ni le gameplay complet ni l'absence de glitches 3D ne sont établis. Le test ciblé Aurora et les 11 tests CTest racine passent. Le patch autonome est `patches/aurora/gekkoaot-bind-group-lifetime-v1.patch` et son application est câblée dans `gekkoaotctl`.

## Shader GX : NBT3 et matrice de texture identité

Le FIFO décrit `GX_NRM_NBT3` par trois indices indépendants N/B/T. Le décodage du bridge en tenait compte, mais le shader Aurora ajoutait à tort un décalage de tranche aux pointeurs B et T après avoir déjà appliqué leurs indices. Pour un format S16, les sources effectives étaient `idxB×stride+6` et `idxT×stride+12` au lieu de `idxB×stride` et `idxT×stride`. Deux tests de génération WGSL échouent avant et passent après la suppression du décalage supplémentaire pour NBT3.

Un second écart concernait `GX_IDENTITY=60` dans l'indice de matrice de texture fourni par sommet : le shader indexait `postex_mtx[60/3]`, soit l'élément 20 d'un tableau de 20 matrices. Le chemin à matrice fixe gérait déjà ce cas. Le shader généré contourne désormais la multiplication pour l'indice identité dynamique ; le test échoue avant et passe après. Les trois tests `gx_shader_tests`, les 206 tests `gx_fifo_tests` et les sept tests `render_worker_tests` passent. Le patch Aurora `patches/aurora/gekkoaot-shader-vertex-v1.patch` s'applique après les patches Aurora existants et est câblé dans `gekkoaotctl`. L'usage de ces deux cas par les scènes HPSS/HPCOS/MKDD capturées n'a pas été établi ; aucun gain visuel en jeu n'est attribué à ce patch à ce stade.

## Coût des tableaux INDEX16 : mesure HPCOS

Le diagnostic opt-in `GEKKOAOT_NATIVE_GX_ARRAY_BASE_TRACE=1` dans le bridge mesure les invalidations et les tailles de fenêtres sans changer le chemin normal. Sur 34 s de HPCOS, le journal `.gekkoaot/audit-v2/array-base-trace-v1/hpcos_growth.log` compte 1 441 792 draws, 2 911 309 agrandissements de fenêtres INDEX16, dont 2 861 180 invalident un snapshot déjà chargé. La somme des préfixes ainsi réuploadables est estimée à 20,95 Go ; le staging a effectué 294 rollovers. Seulement 1 157 commandes `GXInvalidateVtxCache` ont été vues. Les 17 954 réécritures ARRAYBASE identiques sur 49 152 écritures ont écarté environ 35,6 Mo de snapshots, un coût secondaire ici.

La cause est l'agrandissement exact du tableau à chaque nouvel indice maximal dans `PrepareIndexedArrayWindows`, qui invalide `cachedRange`, suivi d'un nouvel upload du préfixe au draw. Arrondir simplement la fenêtre ferait précharger des sommets futurs avant leurs éventuelles écritures CPU et pourrait produire une nouvelle corruption 3D. Aucun tel raccourci n'a été appliqué. Une correction sûre demanderait une capacité staging réservée avec longueur logique exacte, puis l'écriture du seul suffixe nouvellement utilisé avant chaque draw, en respectant les limites de segment et les commandes GPU déjà enregistrées. Il manque un test de sommet modifié dans ce suffixe et un test de rollover avant d'implémenter cette voie.

## BP : masque à usage unique et copies interceptées

Aurora applique le registre BP `0xFE` à une seule écriture BP suivante. Le bridge inspectait cependant les mots bruts `0x49–0x4E` et `0x52` avant ce masquage, puis interceptait `GXCopyDisp` sans transmettre son `0x52` à Aurora. Le masque restait donc armé et pouvait corrompre le registre suivant ; une copie texture rejetée avait le même retour anticipé. Une sonde GPU indépendante via l'ABI publique du bridge reproduit le cas sans dépendre d'une interprétation des formats de profondeur : une écriture `0x4B` masquée laisse la première copie à `0x00010000`, puis un masque appliqué au premier `GXCopyDisp` doit être consommé pour que la seconde écriture `0x4B` déplace la copie suivante à `0x00030000`. Avant correction, les deux adresses sont mal suivies. Le format brut `0xB` combine `RG8` et `Z16R` selon la source de l'EFB ; il ne constitue pas un test valide de rejet de copie en mode profondeur.

Le bridge calcule désormais le mot BP effectif d'après le shadow et le masque avant ses décisions de copie, transmet ce mot aux suivis d'état locaux, et consomme le masque exactement une fois lorsqu'il intercepte ou rejette `0x52`. Les commandes qui passent normalement dans Aurora gardent leur mot FIFO brut et son décodeur applique le masque une seule fois ; les événements PE utilisent également la valeur effective. Le test opt-in `GEKKOAOT_RUN_GPU_TESTS=1 SDL_VIDEODRIVER=x11 python3 tests/hardware/test_bp_mask_display_copy.py` échouait avant et passe après. Dans le premier smoke Double Dash suivant, la piste reste noire à 25 s ; aucune amélioration visuelle n'est attribuée à ce correctif.

Ce smoke s'est arrêté à 30,5 s par `SIGABRT` sans message dans son journal. Le core système `PID 148273` place l'arrêt dans `aurora::gfx::push_verts` durant un draw. `FramePacket::verts` est un `ByteBuffer` externe à capacité fixe de 5 Mio ; son `resize` appelle `abort()` quand l'ajout dépasse la capacité. Un run sous GDB a ensuite franchi 35 s et s'est arrêté volontairement, indiquant un dépassement dépendant de la charge de la frame. La taille demandée et une stratégie de rollover sûre pour sommets/indices restent à établir avant correction ; ce crash n'est pas imputé au masque BP sur la seule base de sa chronologie.

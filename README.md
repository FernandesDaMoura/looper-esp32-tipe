# Pédale Looper Guitare — ESP32 WROOM

**TIPE PSI 2024-2025** · FERNANDES DA MOURA David · n° 53130  
Physique Ondulatoire · Thème : Cycle et Boucle

---

## Problématique

Comment concevoir une pédale looper guitare réalisant des boucles audio de 8 s à 44 100 Hz (352 800 échantillons) avec une latence inférieure à 10 ms sur microcontrôleur Arduino abordable, en implémentant un buffer circulaire `i = (i+1) mod N`, une FSM footswitch ENREG → PLAY → OVERDUB et une chaîne ADC → filtrage anti-aliasing → DAC ?

---

## Structure du dépôt

```
looper-esp32-tipe/
├── README.md
├── code/
│   └── looper_wroom_v3/
│       └── looper_wroom_v3.ino     ← code Arduino principal
├── schema/
│   ├── schema_looper.tex           ← source LaTeX/CircuitTikZ
│   └── schema_looper.pdf           ← schéma compilé
├── simulation/
│   └── simulation_looper.py        ← simulation Python (SNR, latence, overdub)
├── data/
│   ├── latence_mesures.csv         ← mesures oscilloscope
│   └── snr_overdub_mesures.csv     ← mesures SNR overdub (5 couches)
└── docs/
    └── (présentation TIPE)
```

---

## Matériel requis

| Composant | Référence | Rôle |
|---|---|---|
| Microcontrôleur | **ESP32 WROOM** (pas Nano ESP32) | CPU + ADC + DAC + timer |
| AOP | TL071 ou LM358 | Préampli guitare gain ×47 |
| R 100 kΩ × 2 | — | Pont diviseur biais 1,65 V |
| R 1 kΩ × 3 | — | Entrée AOP, filtre RC, sortie DAC |
| R 47 kΩ | — | Contre-réaction AOP |
| C 100 nF | — | Filtre anti-aliasing |
| C 10 µF | électrolytique | Découplage biais |
| Jack femelle 6,35 mm × 2 | — | Entrée guitare, sortie ampli |
| Footswitch momentané NO | — | Contrôle FSM |

---

## Câblage ESP32 WROOM

| Broche GPIO | Fonction | Composant |
|---|---|---|
| **GPIO 34** | ADC entrée (input only) | Sortie filtre anti-aliasing |
| **GPIO 25** | DAC sortie hardware | Jack OUT via R 1 kΩ |
| **GPIO 2** | Footswitch (INPUT_PULLUP) | Bouton poussoir NO vers GND |
| **3,3 V** | Alimentation AOP | VCC pont diviseur |
| **GND** | Masse | Commune |

> ⚠ GPIO 34 est *input only* sur ESP32 WROOM — ne jamais configurer en sortie.  
> ⚠ GPIO 25 est le DAC hardware (avec GPIO 26) — ne pas utiliser GPIO 26 simultanément.

---

## Installation et compilation

1. Installer [Arduino IDE](https://www.arduino.cc/en/software) ≥ 2.0
2. Ajouter le support ESP32 dans le gestionnaire de cartes :  
   URL : `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Sélectionner la carte : **ESP32 Dev Module**
4. Ouvrir `code/looper_wroom_v3/looper_wroom_v3.ino`
5. Compiler et téléverser

**Paramètres importants dans le code :**

```cpp
#define FS        44100UL   // Fréquence d'échantillonnage
#define LOOP_SEC  4         // Durée boucle (changer en 8 avec PSRAM)
#define PIN_ADC   34        // GPIO ADC entrée
#define PIN_DAC   25        // GPIO DAC sortie
#define PIN_SW     2        // GPIO footswitch
```

---

## Machine à états (FSM)

```
IDLE ──(court)──► ENREG ──(court)──► PLAY ◄──(court)──► OVERDUB
  ▲                  │                 │
  │                  │ (auto : buf     │
  └──────────────────┴── plein v3)     │
  └──────── appui long > 600 ms : RESET depuis tous les états ─┘
```

| État | Action |
|---|---|
| **IDLE** | Dry-through : signal direct sans traitement |
| **ENREG** | Écriture buffer, `i = (i+1) % N` |
| **PLAY** | Lecture buffer en boucle |
| **OVERDUB** | Mixage additif signal + buffer (irréversible) |

---

## Résultats principaux

| Mesure | Simulé | Mesuré | Objectif MCOT |
|---|---|---|---|
| Latence @ 44,1 kHz | 22,7 µs | **25,1 µs** | < 10 ms |
| SNR 1 couche (8 bits) | 49,9 dB | **47,9 dB** | ≥ 60 dB |
| SNR 5 couches overdub | 35,3 dB | **30,4 dB** | Mesure qualitative |

**Écart latence :** +2,4 µs constant → overhead conversion ADC 12 bits SAR (~2 µs à 80 MHz), non modélisé dans `T_ISR = 1/Fs`.

**Aberration à 22 kHz :** 51,3 µs mesurés (vs 45,4 µs simulés) — instabilité thermique de l'oscilloscope lors de cette acquisition, valeur écartée pour le calcul de R².

---

## Limites et perspectives

| Limite | Problème | Solution |
|---|---|---|
| **L1** — DAC 8 bits | SNR 47,9 dB < 60 dB | MCP4921 SPI 12 bits (~2 €) → 74 dB |
| **L2** — SRAM 520 ko | 689 ko requis pour 8 s | PSRAM externe + `ps_malloc` |
| **L3** — Filtre RC fc=1,6 kHz | Harmoniques guitare atténuées | R=100 Ω → fc≈16 kHz |
| **L4** — Buffer plein | Écrasement silencieux (v1/v2) | Correction v3 : transition auto |

---

## Simulation Python

```bash
cd simulation
pip install numpy matplotlib
python simulation_looper.py
```

Génère `simulation_resultats.png` avec 4 graphiques : SNR vs bits, latence vs Fs, dégradation overdub, SNR vs Fs.

---

## Compilation du schéma LaTeX

```bash
cd schema
pdflatex schema_looper.tex
```

Nécessite : `texlive-full` ou `MiKTeX` + package `circuitikz`.

---

## Bibliographie

| Réf. | Source |
|---|---|
| [1] | Arduino SA — ESP32 WROOM Technical Reference |
| [2] | Boss Corporation — RC-500 Loop Station Manual |
| [5] | TC Electronic — Ditto Looper Specifications |
| [6] | Roads C. — Microsound, MIT Press, 2004 |
| [8] | Lyons R.G. — Understanding DSP, 3e éd., Prentice Hall, 2011 |
| [10] | Nyquist H. — Certain Topics in Telegraph Transmission Theory, 1928 |

---

## Licence

Code source distribué sous licence **MIT**.  
Schéma LaTeX et données de mesure : **CC BY 4.0**.

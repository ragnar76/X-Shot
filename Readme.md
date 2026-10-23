# X-Shot 1.0

Screenshot-Accessory für den Atari ST/STE/TT/Falcon und kompatible Rechner
(EmuTOS, FireBee, …). X-Shot speichert den ganzen Bildschirm, einen
aufgezogenen Bereich oder ein einzelnes Fenster als **PNG** oder **TIFF**.

- läuft in **jeder Auflösung und Farbtiefe** (1 Bit bis 32 Bit, auch 1920×1080 und mehr)
- kommt **ohne Fremdbibliotheken** aus (PNG- und TIFF-Encoder sind selbst geschrieben)
- ein einzelnes `.acc`, keine Zusatzdateien, keine Einstellungsdatei

Web: <https://theragnarbay.org/xshot/>  
Autor: Bernd Müller – <ragnar76@googlemail.com>

---

## Installation

1. `xshot.acc` in das Hauptverzeichnis des Startlaufwerks kopieren (z. B. `C:\`).
2. Rechner neu starten. Im **Desk-Menü** erscheint der Eintrag **X-Shot**.

Es gibt Varianten für verschiedene CPUs. Die 68000-Variante läuft auf jedem
68k-Atari, die anderen sind für die jeweilige CPU erzeugt:

| Ordner           | CPU                                 |
|------------------|-------------------------------------|
| `dist/68000/`    | 68000 (ST, STE, Mega, …)            |
| `dist/68030/`    | 68030 (TT, Falcon, 030-Beschleuniger) |
| `dist/68040/`    | 68040                               |
| `dist/68060/`    | 68060                               |
| `dist/firebee/`  | ColdFire V4e (FireBee)              |

X-Shot ist **nur** als Accessory gebaut. Ein Doppelstart wird nicht geprüft –
bitte nur eine Kopie installieren.

## Bedienung

Das Desk-Menü öffnet ein kleines Fenster mit vier Buttons. Der Closer des
Fensters schließt es wieder, das Accessory bleibt geladen.

| Button               | Kürzel | Funktion                                             |
|----------------------|--------|------------------------------------------------------|
| **Ganzer Bildschirm**| Alt+G  | gesamten Bildschirm aufnehmen                        |
| **Bereich auswählen**| Alt+B  | Rahmen aufziehen und diesen Bereich aufnehmen        |
| **Fenster aufnehmen**| Alt+F  | ein Fenster anklicken und dieses aufnehmen           |
| **Über**             | Alt+U  | Infodialog mit Kurzanleitung                         |

(Das Kürzel ist im Button jeweils am ersten Buchstaben unterstrichen. Die
Tastenkürzel funktionieren, wenn das X-Shot-Fenster oben liegt.)

### Ganzer Bildschirm
Das Fenster schließt sich, der Bildschirm wird neu gezeichnet und dann
aufgenommen. Der Mauszeiger ist dabei nicht im Bild.

### Bereich auswählen
Mit gedrückter **linken Maustaste** einen Rahmen aufziehen (in jede Richtung),
loslassen nimmt auf. **Rechte Maustaste** bricht ab.

### Fenster aufnehmen
Der Mauszeiger markiert das Fenster darunter mit einem Rahmen. **Linksklick**
nimmt es samt Rahmen und Titelleiste auf, **rechte Maustaste** bricht ab. Ein
Klick auf den leeren Desktop bricht ebenfalls ab. Wird das Fenster teilweise
von einem anderen verdeckt, ist das andere im Bild zu sehen (aufgenommen wird,
was auf dem Bildschirm steht).

### Speichern
Nach der Aufnahme erscheint der Dateiauswahldialog.

- **Format:** Endet der Dateiname auf `.TIF` oder `.TIFF`, wird TIFF geschrieben,
  sonst PNG. Ohne Endung wird `.PNG` angehängt.
- **Namensvorschlag mit Nummer:** X-Shot sucht im gewählten Ordner nach
  bestehenden Dateien und schlägt die nächste freie Nummer vor:

  | Aufnahme          | Vorschlag      |
  |-------------------|----------------|
  | Ganzer Bildschirm | `SHOT0001.PNG` |
  | Bereich           | `AREA0001.PNG` |
  | Fenster           | `WIN0001.PNG`  |

  Jede Art zählt für sich. Als Endung wird das zuletzt gewählte Format
  vorgeschlagen.
- **Überschreiben:** Gibt es die Datei schon, fragt X-Shot nach. Bei „Nein"
  öffnet sich der Dateiauswahldialog erneut.
- Während des Schreibens zeigt der Mauszeiger die Sanduhr.

## Hinweise und Grenzen

- **PNG und TIFF sind unkomprimiert.** PNG wird mit „stored"-Deflate-Blöcken
  geschrieben: gültig und von allen Programmen lesbar, aber nicht kleiner als
  die Rohdaten. Das hält das Schreiben schnell, auch auf langsamen Rechnern.
  Beispiel: 1920×1080 mit 32 Bit ergibt rund 6 MB.
- **Speicher:** Vor jeder Aufnahme prüft X-Shot, ob genug zusammenhängender
  Speicher frei ist. Wenn nicht, erscheint eine Meldung mit dem Bedarf und dem
  freien Speicher. Dann hilft ein kleinerer Bereich.
- **Farbformat bei 16/24/32 Bit:** Das Pixelformat des Bildschirms wird vor
  jeder Aufnahme gemessen (der Pixel oben links wird kurz in Schwarz, Rot, Grün
  und Blau gezeichnet, ausgelesen und danach wiederhergestellt). Dadurch stimmen
  die Farben auch bei Grafikkarten mit ungewöhnlichem Format. Schlägt die
  Messung fehl, erscheint ein Hinweis und es wird eine Standardannahme benutzt.
- **Bis 8 Planes** werden als PNG/TIFF mit Palette (1, 2, 4 oder 8 Bit)
  gespeichert.
- **Offene Menüs** lassen sich nicht aufnehmen.
- Voraussetzung ist ein AES mit `fsel_exinput` (TOS ab 1.04).

## Selbst bauen

Benötigt wird eine Cross-Toolchain `m68k-atari-mint-gcc` (getestet mit 4.6.4)
mit MiNTLib und gemlib (`-lgem`).

```
make              # alle Varianten nach dist/<variante>/xshot.acc
make 68000        # nur eine Variante (68000, 68030, 68040, 68060, firebee)
make info         # prüft, ob libgem.a für jede CPU vorhanden ist
make release      # dist/ als xshot-<version>.zip packen
make clean
```

Die Versionsnummer steht im `Makefile` (`VERSION`) und wird in den
Über-Dialog übernommen.

### NewTOS (experimentell)

```
make newtos       # ~/src/NewTOS/sdk/bin/newtos-cc -Wall -nap -o shot-pi.nac shot.c
```

Das Target ist nicht Teil von `make all`. Ob der Quelltext dort ohne Anpassung
durchläuft, ist offen: An einigen Stellen werden Zeiger in `long` abgelegt
(z. B. beim Fenstertitel, bei `ob_spec` und in der Speicherverwaltung), was bei
64-Bit-Zeigern angepasst werden muss.

## Getestet

- EmuTOS (Hatari)
- FireBee, 1920×1080 mit 32 Bit
- Echte TOS-Hardware: noch offen

## Lizenz
Copyright (C) 2026 Bernd Müller
Licensed under the GNU GPL v3.0 or later (SPDX: GPL-3.0-or-later).
See  for the full text.

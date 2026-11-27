# Auxiliary Changes — Blitter, CV1000 Timing & NV2A Renderer Fixes

Dokument podsumowuje zestaw poprawek stabilności, poprawności kolejności operacji oraz optymalizacji pod Pentium III (Original Xbox / `nxdk`) wprowadzonych w `blitter.cpp`, `cv1000.h`, `cv1000.cpp` oraz `nv2a_cv1k_renderer.h`.

---

## 1. Poprawki poprawności (Bugfixes)

### A. Bezpieczny O(1) fast-forward pollingu blittera (`cv1000.h`, `cv1000.cpp`)
- **Problem:** W `Cv1000::Read()` licznik `m_busyPollCount` rósł przy każdym odczycie rejestru statusu blittera (`0x18000010`) bez sprawdzania, czy odczyty pochodzą z tej samej ciasnej pętli pollingu, i nie zerował się między klatkami ani przy wejściu w przerwanie `IRQ2` (VSync). Gdy ciężka lista z klatki `N` kończyła się dopiero w klatce `N+1`, pojedynczy nieblokujący odczyt `0x18000010` w ISR lub w innej funkcji gry natychmiast przewijał `cpu.cycles` do `m_blitterDone` i przedwcześnie zwracał stan `READY (0x10)`.
- **Poprawka:**
  - Dodano pola `m_lastBusyPollPc` i `m_lastBusyPollCycle` (zerowane w `Reset()` i przy starcie nowej listy blittera).
  - Fast-forward uruchamia się wyłącznie wtedy, gdy kolejny odczyt `0x18000010` pochodzi z tego samego `cpu.PC` w oknie `<= 64` cykli SH-3.
  - Gdy `noSlowdown` zostanie włączone w locie (przycisk `LT`), ewentualny trwający stan `blitter.busy` jest natychmiast zwalniany.

### B. Zachowanie kolejności uploadów w `Blitter::Upload()` (`blitter.cpp`)
- **Problem:** Ścieżka zero-copy `useDirectUpload` sprawdzała `native->draws.empty()` zamiast `native->steps.empty()`. Jeśli w tej samej klatce wcześniejszy upload wychodził poza krawędź VRAM (`nativeW != dimX` lub `nativeH != dimY`) i trafiał do kolejki `native->uploads`, kolejny nieprzycięty upload zapisywał się od razu do stron VRAM przed nim, odwracając kolejność przy nakładających się obszarach.
- **Poprawka:** Zmieniono warunek na `native->steps.empty()`.

### C. Kolejność sprawdzania przepełnienia VB w `Nv2aCv1kRenderer::ProcessDraw()` (`nv2a_cv1k_renderer.h`)
- **Problem:** Sprawdzenie `if (vbCount_ + 4u > kMaxVertsPerFrame)` wywoływało `FlushMidFrameAndResetShelf()` już *po* odnalezieniu lub zaalokowaniu wpisu `slot` w `rectHash_`. Ponieważ flush przełącza `activeBank_`, podbija epokę drugiego banku i przesuwa `atlasPhase_`, wpis `slot` mógł stracić spójność z aktywnym bankiem atlasu.
- **Poprawka:** Przeniesiono warunek `vbCount_ + 4u > kMaxVertsPerFrame` przed lookup w `rectHash_`.

---

## 2. Rozmiar binarki (`default.xbe`) i pamięć RAM

### Wyłączenie 260 martwych instancji `Blit<>` pod `-DXBOX_NO_SW_VRAM` (`blitter.cpp`)
- **Problem:** Mimo flagi `-DXBOX_NO_SW_VRAM` (gdzie `vram` jest pustym wektorem, a `rasterize == false`), kompilator nadal generował tablicę 260 funkcji `kBlits[65 * 4]` oraz tablice LUT `g_mul`, `g_rev`, `g_add`.
- **Poprawka:** Otoczono cały software'owy rasteryzer (`g_mul`/`g_rev`/`g_add`, `SourceTerm`, `DestTerm`, `Blit<>`, `MakeBlits`, `kBlits` oraz gałąź `if (rasterize)` w `DrawOp()`) dyrektywą `#ifndef XBOX_NO_SW_VRAM`. Zmniejsza to rozmiar `default.xbe`, oszczędza pamięć fizyczną na 64 MB Xboxie i przyspiesza linkowanie ThinLTO.

---

## 3. Optymalizacje hot-path pod Pentium III

1. **32-bitowa arytmetyka `clocks` w `Blitter::DrawOp()` (`blitter.cpp`):** Zastąpiono 64-bitowe dzielenia ze znakiem (`int64_t(w) * h / 4`, `int64_t(dw) * dh / 2`) bit-exact przesunięciami `uint32_t` (`>> 2`, `>> 1`).
2. **Jednorazowe maskowanie 20-bajtowego polecenia draw w `Blitter::DrawOp()` (`blitter.cpp`):** Dla typowego przypadku bez zawijania na końcu RAM (`base + 20u <= ramMask + 1u`) wszystkie 10 słów polecenia czytane są z jednego wskaźnika `ram + base` zamiast 10 osobnych wywołań `Word(address)`.
3. **Szybka pętla 2-pikselowa BE→LE w `Blitter::Upload()` i `Nv2aCv1kRenderer::ApplyDirectUpload()`:** Dla wierszy niezawijających się na granicy RAM SH-3 konwersja Big-Endian → Little-Endian przetwarza po 2 piksele (`uint32_t` + swap sąsiednich bajtów `((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu)`) bez maskowania `ramMask` na każdym pikselu.
# Translations

One plain-text file per language: `lang\<code>.lang` (UTF-8). It is picked up by name, so a new file shows in Settings > Language
after **Reload** (or a restart). Files in `%APPDATA%\PrismFS\Languages\` (**Open languages folder** in Settings) override the ones beside the exe.

```
@name = Deutsch                 # shown in Settings, in its own language
@plural = one-other             # one-other (default) | none (zh ja ko) | french | slavic (ru uk pl cs)
"Compact density" = "Kompakte Dichte"
"Hello \"you\"\n" = "..."       # \" \\ \n \t are the only escapes
"{} item|{} items" = "{} Element|{} Elemente"   # plural: one form per rule slot, separated by |
```

- The left side is the English text exactly as in the code. An empty or missing translation shows the English text.
- Keep `{}` (and `{:.1f}` etc.) in the translation: they are filled in at run time. Their order may change only if you use `{0}`, `{1}`.
- Lines starting with `#` are comments.
- New language: `tools\lang.ps1 -New de -Name Deutsch`, then fill in the quotes on the right.
- After the code changed: `tools\lang.ps1 -Update` adds the new strings (empty) to every file and lists the unused ones at the end.
  Without `-Update` it only rewrites `template.lang` and reports what is missing.
- Plural slots: `one-other` = 1 / rest; `french` = 0-1 / rest; `slavic` = 1, 21... / 2-4, 22... / rest; `none` = a single form.
- Fonts: Latin, Greek and Cyrillic are built in; other scripts are added to the font automatically when the language loads.

# Icons8 Color Icons — License Notice

The color icon PNG files in this directory are sourced from [Icons8](https://icons8.com)
under their **Universal Multimedia Licensing Agreement** (paid license).

## License Terms

- **License type:** Icons8 Paid Subscription
- **Usage:** Desktop application icons for Impossible OS
- **Permitted:** Modification, embedding in software, commercial use
- **Prohibited:** Redistribution as standalone files, sublicensing

## Why are the PNG files not in this repository?

The Icons8 license **prohibits redistribution of Licensed Materials as standalone files**.
The PNG icon files are therefore excluded from version control via `.gitignore`.

## How to obtain the icons

If you are building Impossible OS and need the color icons:

1. Create a paid [Icons8](https://icons8.com) account
2. Download the icons listed in `icon_store.h` (system_icon_t enum, color section)
3. Use the **Fluent Color** style for consistency with the monochrome Fluent icons
4. Download at sizes: 48, 72, 128, 256px
5. Place files in `resources/icons/apps/{size}/` directories

The OS will still build and run without these icons — the icon store gracefully
returns NULL for missing color icons, and the system falls back to monochrome
alternatives where possible.

## Full License Text

See: https://intercom.help/icons8-7fb7577e8170/en/articles/5534926-universal-multimedia-licensing-agreement

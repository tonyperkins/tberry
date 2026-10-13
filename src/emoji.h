#pragma once
// Emoji from the SD card. Each emoji is a small image file /emoji/<codepoint>.bin. When the files
// are present they are drawn inline in message text; when they are not, the emoji are simply left
// out. No emoji is ever shown as an empty box.
#include <lvgl.h>
#include <stddef.h>

void emoji_init();                       // after the SD card is mounted and LVGL is up
bool emoji_pack_present();               // at least the pack folder exists on the card
const lv_font_t* emoji_font(const lv_font_t* text_font);   // text font + inline emoji, or text_font
// Copy `src` to `dst`. With keepAvailable, emoji that have an image on the card are kept and the
// rest dropped (for fonts made by emoji_font). Without it every emoji is dropped (for fonts that
// cannot draw them). Plain text is untouched.
void emoji_filter(const char* src, char* dst, size_t n, bool keepAvailable = true);

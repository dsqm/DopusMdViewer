# DopusMdViewer POC sample

This file exists so the plugin has something real to identify and load.
The POC skeleton never parses it — it only reports the path and the byte count.

## Headings

### Level three heading

## Lists

- first item
- second item
  - nested item
- third item

1. ordered one
2. ordered two
3. ordered three

- [ ] unchecked task
- [x] checked task

## Code block

```cpp
// The viewer does not run this; it is here to prove long lines survive a copy.
int main() {
    return 0;
}
```

```powershell
Get-ChildItem -LiteralPath "C:\Users\momo\Downloads\Directory Opus\Viewers" |
    Select-Object Name, Length
```

## Inline formatting

**bold**, *italic*, ~~strikethrough~~, `inline code`, and a [link](https://example.com).

## Table

| Plugin        | Version | Status |
| ------------- | ------- | ------ |
| DopusMdViewer | 0.1.0.0 | POC    |

## Math (kept as raw source)

Inline: $E = mc^2$

Block:

$$
\int_{0}^{\infty} e^{-x^2}\,\mathrm{d}x = \frac{\sqrt{\pi}}{2}
$$

## Blockquote

> Quoted text, with a trailing hard break.  
> Second line of the quote.

---

End of sample.

# MIKIR-OS

アセンブリ（IPL・ローダ）の上に **C カーネル** を載せた OS です。

## 構成

| ファイル | 言語 | 説明 |
|----------|------|------|
| **ipl.nas** | asm | ブートセクタ。セクタ 2〜5 を 0x8000 に読み、ローダへジャンプ。 |
| **loader.nas** | asm | A20 有効化・GDT・プロテクトモード移行後、**kernel_main()** を呼ぶ。 |
| **kernel/kernel.c** | **C** | 32bit カーネル。VGA (0xb8000) に "MIKIR-OS (C kernel)" を表示。 |
| **link.ld** | - | ローダとカーネルのリンクスクリプト。 |

## 必要環境

- **nasm**: `brew install nasm`
- **gcc**（32bit 対応）: macOS では `brew install gcc` や x86 用ツールチェーンが必要な場合あり
- **make**
- **QEMU**: `brew install qemu`

## ビルド

```bash
make
```

生成: `mikiros.img`（1.44MB フロッピーイメージ）

## 実行

```bash
make run
```

起動後、画面上部に「MIKIR-OS (C kernel)」が表示されます（32bit プロテクトモードで C が動作）。

## ロードマップ

[ROADMAP.md](ROADMAP.md) を参照。Phase 2 で C のシェル（コマンドループ）を追加予定です。

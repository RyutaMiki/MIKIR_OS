# Chocola

BIOS ブートからカーネルまで全て自作した x86 OS です。

## これは何？

一般的な PC では、起動時に複数のソフトウェアが順番に実行されます。
Chocola では BIOS（QEMU 付属）以外の **全てを自作** しています。

```
┌────────┐    ┌────────────┐    ┌──────────────┐    ┌──────────────┐
│  BIOS  │ →  │  ipl.nas   │ →  │ loader.nas   │ →  │  kernel.c    │
│ (QEMU) │    │ ブート     │    │ 32bit 切替   │    │ シェル       │
│        │    │ ローダ     │    │ GDT/IDT/PIC  │    │ ドライバ     │
└────────┘    └────────────┘    └──────────────┘    └──────────────┘
  既製品        自作 ←──────────── 自作 ──────────────→ 自作
```

| 層 | 一般的な PC | Chocola |
|----|-----------|---------|
| ファームウェア | BIOS / UEFI | QEMU の SeaBIOS（既製品） |
| ブートローダ | GRUB / Windows Boot Manager | **ipl.nas**（自作） |
| OS ローダ | Linux の setup.bin 等 | **loader.nas**（自作） |
| カーネル | Linux kernel / NT kernel | **kernel.c**（自作） |

※ UEFI ではなく旧来の **BIOS ブート** 方式を採用しています。

## ファイル構成

| ファイル | 言語 | 役割 |
|----------|------|------|
| ipl.nas | asm | ブートセクタ（512B）。ディスクからカーネルを読み込み、ローダへジャンプ。 |
| loader.nas | asm | A20 有効化、GDT/IDT 設定、16bit→32bit 切替、ISR スタブ。 |
| kernel/kernel.c | C | シェル、VGA ドライバ、PIC/PIT、キーボード割り込み、ATA ディスク読み取り。 |
| link.ld | - | リンカスクリプト（16bit/32bit セクションの VMA/LMA 分離）。 |
| mkfs.py | Python | ビルド時にディスクイメージへテストファイルを書き込み。 |
| Makefile | - | ビルド・実行の自動化。 |

## 機能

- **シェル**: `C:\>` プロンプトでコマンド入力
- **コマンド**: `help`, `ver`, `clear`, `echo`, `uptime`, `dir`, `ls`, `type`, `cat`
- **割り込み駆動**: キーボード（IRQ1）、タイマー（IRQ0, 100Hz）
- **ATA PIO**: IDE ディスクからセクタ読み取り
- **ファイル一覧**: ディスク上のファイルを `dir` で表示、`type` で中身を表示

## 動かし方

### 1. 必要なツールを入れる（macOS）

```bash
brew install nasm qemu i686-elf-gcc
```

- **Apple Silicon (M1/M2/M4)**: `i686-elf-gcc` が必須（`gcc -m32` では x86 向けが出せない）
- Intel Mac: `gcc -m32` でも可（Makefile が自動判定）

### 2. ビルドと実行

```bash
cd MIKIR_OS
make        # mikiros.img を生成
make run    # QEMU で起動
```

起動すると「Chocola Ver0.1」と `C:\>` プロンプトが表示されます。

### 3. 試せるコマンド

```
C:\>help          コマンド一覧
C:\>dir           ファイル一覧
C:\>type hello.txt  ファイル表示
C:\>uptime        起動時間
C:\>ver           バージョン
C:\>clear         画面クリア
C:\>echo hello    テキスト表示
```

終了: QEMU ウィンドウを閉じる。

## ロードマップ

[ROADMAP.md](ROADMAP.md) を参照。Phase 6 以降でディスク書き込み、FAT12、メモリ管理、マルチタスク、GUI へと進みます。

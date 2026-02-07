# MIKIR-OS

アセンブリ（IPL・ローダ）の上に **C カーネル** を載せた OS です。

## 構成

| ファイル | 言語 | 説明 |
|----------|------|------|
| **ipl.nas** | asm | ブートセクタ。セクタ 2〜5 を 0x8000 に読み、ローダへジャンプ。 |
| **loader.nas** | asm | A20 有効化・GDT・プロテクトモード移行後、**kernel_main()** を呼ぶ。 |
| **kernel/kernel.c** | **C** | 32bit カーネル。VGA (0xb8000) に "MIKIR-OS (C kernel)" を表示。 |
| **link.ld** | - | ローダとカーネルのリンクスクリプト。 |

## 動かす手順（手元で実行する場合）

**1. 必要なツールを入れる（macOS）**

```bash
brew install nasm qemu
# カーネル用の x86 32bit コンパイラ（どちらか）
brew install i686-elf-gcc   # Apple Silicon (M1/M2/M4) の場合はこちら
# または（Intel Mac など gcc -m32 が使える環境）
brew install gcc
```

- **Apple Silicon (M1/M2/M4)**: `gcc -m32` では x86 向けが出せないため、**i686-elf-gcc** が必須です。
- Homebrew で権限エラーが出る場合は、表示された `sudo chown` を実行してから再度 `brew install`。

**2. ビルドと実行**

```bash
cd MIKIR_OS
make
make run
```

- `make` で `mikiros.img` ができる。
- `make run` で QEMU が起動する（第1ディスクとして `mikiros.img` を接続）。画面上部に **「MIKIR-OS (C kernel)」** が出れば成功。
- 終了: QEMU ウィンドウで `Ctrl+C` または閉じる。

**注意**: Apple Silicon (M1/M2/M4) では `brew install i686-elf-gcc` を先に入れておくと、Makefile が自動でクロスコンパイラを使います。

## 必要環境（一覧）

- **nasm**: アセンブラ
- **gcc**（32bit 対応）: カーネル用
- **make**
- **QEMU**: 実行確認用

## ロードマップ

[ROADMAP.md](ROADMAP.md) を参照。Phase 2 で C のシェル（コマンドループ）を追加予定です。

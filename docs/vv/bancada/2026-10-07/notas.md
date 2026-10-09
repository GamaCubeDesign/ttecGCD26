# Bancada — 07/10/2026

Primeira sessão com a Pi. Roteiro:
[2026-10-05-roteiro-testes-raspberry.html](../../../relatorios/2026-10-05-roteiro-testes-raspberry.html).

## Montagem

- Pi Zero 2 W, Raspberry Pi OS de 64 bits (decisão de 07/10, que substitui a
  de 32 bits de 05/10).
- Rede: hotspot do celular, configurado na Pi com `nmcli`; o PC no mesmo
  hotspot. Nome na rede: `gamapi.local`.
- Monitor no micro-HDMI da Pi, para acompanhar a rede (`nmcli device status`)
  sem depender do ssh.
- Rádios e SDR ainda não ligados.
- **16:30 — troca de rede.** O hotspot do celular estava lento demais; o PC
  passou a ser o ponto de acesso (`gama-bancada`, NetworkManager *shared*),
  repassando à Pi a internet cabeada da universidade. Pi em 10.42.0.75, sinal
  −53 dBm, HTTP ok, hora sincronizada; o `ping` para fora não volta (ICMP
  bloqueado pela universidade). Procedimento completo:
  [README, "Rede da bancada"](../README.md#rede-da-bancada-o-pc-como-ponto-de-acesso).

## Etapa 1

Conectar e preparar a Pi. **Concluída.**

- Acesso sem senha: a chave ssh pessoal do PC tem passphrase e não há
  `ssh-agent`, então uma chave só para a Pi, sem passphrase, foi criada no PC
  e associada a `gamapi.local` no `~/.ssh/config` (`IdentitiesOnly yes`). As
  outras conexões ssh do PC (git) seguem com a chave de sempre.
- `local.mk`: `PI_HOST = pi@gamapi.local`.
- `make pi-provision` rodado; `poppler-utils` e `sysstat` instalados à parte
  (o `provision` ainda não os inclui); Pi reiniciada.

Conferência, às 15:50 (-03):

| Item | Resultado | |
|---|---|---|
| Modelo | Raspberry Pi Zero 2 W Rev 1.0 | ✅ |
| Arquitetura | `arm64` | ✅ |
| Sistema | Debian 13 (trixie), kernel 6.18.34+rpt-rpi-v8 | |
| Data | correta (Wed 7 Oct 15:50:23 -03 2026, igual à do PC) | ✅ |
| SPI | `/dev/spidev0.0` e `/dev/spidev0.1` | ✅ |
| Ferramentas | gcc 14.2.0, CMake 3.31.6, git, python3, rsync, tmux, `pdftotext`, `pidstat` | ✅ |
| Grupos do `pi` | `spi`, `gpio`, `plugdev` (entre outros) | ✅ |
| Usuário `gama` | existe, em `spi` e `gpio` | ✅ |
| Memória | 415 MB total, 262 MB disponíveis; swap de 415 MB em zram | |
| Alimentação | `throttled=0x0` | ✅ |
| Temperatura | 48,3 °C, ociosa, sem SDR | |

## Observações

- Compilador diferente do PC: gcc 14.2 na Pi, 16.2 no PC — mais um motivo
  para a etapa 2.
- Memória curta para o `make check` (dois builds, um com sanitizers): se um
  processo morrer sem mensagem, repetir com `JOBS=1`.
- A data está certa porque a Pi tem internet pelo hotspot. Na prova não terá:
  a hora vem do `SET_TIME` da ground (ADR-0009).
- O `dump1090-fa` ainda não está instalado (etapa 5.1); nem o build nem o
  `provision` o instalam.

## Etapa 2

O código na Pi. **Em andamento.**

### `make pi-test` — 16:08

Primeira cópia e primeiro build na Pi (`~/ttec`, RelWithDebInfo, `-j2`).

| Item | Resultado | |
|---|---|---|
| Compilador | GNU 14.2.0, Python 3.13.5 | |
| Avisos na compilação | nenhum `warning:` | ✅ |
| Testes | **23 de 23** (`100% tests passed`) | ✅ |
| Tempo | 1 min 26 s ao todo (sync, configuração, build, testes); ctest 21,6 s | |

Os mais lentos no ctest: `test_adsb_chain` 8,1 s, `test_ttcd_integration`
4,5 s, `test_adsbd_integration` 4,0 s, `ground_aeronaves` 2,9 s — os que rodam
os binários reais em tempo real.

### `make check` — 16:10, **não roda na Pi como está**

Log: `check-pi.txt`.

| Parte | Resultado | |
|---|---|---|
| Build normal + ctest | 23 de 23, de novo | ✅ |
| Build com sanitizers (Debug, avisos como erro) | **compila sem nenhum aviso** | ✅ |
| Testes com ASan + UBSan | 20 testes em C abortam em 0,02 s, antes de rodar | ❌ plataforma |
| Citações do regulamento | não chegou a rodar (o `make` parou no erro) | |

Causa: o AddressSanitizer do gcc 14 em arm64 reserva memória em
`0x500000000000` (80 TiB) e o kernel da Pi Zero 2 W dá a cada processo um
espaço de endereços de **39 bits** (512 GiB; a pilha fica em `0x7ff2ee8000`).
Mensagem: `AddressSanitizer: CHECK failed: sanitizer_allocator_primary64.h:131
... (0x500000000000, 0xfffffffffffffff4)`. **Não é defeito do código:** o ASan
não funciona nesta plataforma, e segue valendo só no PC.

### Só UBSan, à mão — 16:12

Build à parte em `build-ubsan/` (Debug, avisos como erro,
`-fsanitize=undefined -fno-sanitize-recover=undefined`), sem mudar o
repositório. Log: `check-ubsan-pi.txt`.

| Parte | Resultado | |
|---|---|---|
| Compilação | sem avisos | ✅ |
| Testes | **22 de 23**; falha o `test_tm` | ❌ achado |
| Erros de UBSan (`runtime error`) | nenhum | ✅ |
| Citações (`make requirements`, 16:20) | 71 de 71 | ✅ |

**Achado: o `test_tm` compara bytes de enchimento das structs.** O grupo
"stat: round-trip preserves every counter" (`tests/test_tm.c:235`) compara
`gama_stat_t` inteira com `memcmp`. A struct tem 28 bytes, e os 2 bytes depois
de `aircraft_tracked` (offsets 10 e 11) são enchimento, que o decodificador não
escreve. Na Pi, o byte 11 de `out` veio com lixo da pilha (`0x97`, esperado
`0x00`):

```
got:      32 EF 00 00 E4 E9 00 00 14 00 00 97 1F 01 00 00 ...
expected: 32 EF 00 00 E4 E9 00 00 14 00 00 00 1F 01 00 00 ...
```

Todos os campos batem; **o codec está certo** — o formato no fio continua
fixado pelo `test_vectors`, que passou. O defeito é do teste: o C não garante o
valor do enchimento, e no PC ele passava por sorte. O grupo do HK
(`test_tm.c:211`) tem o mesmo padrão: `gama_hk_t` tem 32 bytes para 25 de
campos. As duas comparações de `test_ipc_codec.c` são de structs sem
enchimento.

### Correções — 16:30

- `tests/test_tm.c`: os grupos do HK e do STAT comparam campo a campo, não
  mais a struct inteira.
- `make asan` escolhe os sanitizers por `SANITIZERS=`: `address,undefined` no
  PC, só `undefined` na Pi (`Makefile`, `CMakeLists.txt` com
  `TTEC_SANITIZERS`; `README.md`, `AGENTS.md` e o roteiro explicam por quê).

### `make check` de novo — 16:35, **passou**

| Onde | Build normal | Build com sanitizers | Citações | Log |
|---|---|---|---|---|
| PC (gcc 16.2) | 23/23 | 23/23, ASan + UBSan, sem avisos | 71/71 | `check-pc.txt` |
| Pi (gcc 14.2) | 23/23 | 23/23, UBSan, sem avisos, nenhum `runtime error` | 71/71 | `check2-pi.txt` |

**Etapa 2 concluída.** O código compila sem avisos e passa em todos os testes
na Pi, em arm64, com o compilador dela.

Observação para depois: o `make asan` não usa `-fno-sanitize-recover`, então
um erro do UBSan é impresso mas não reprova o teste — hoje a conferência é o
`grep "runtime error"` no log (zero na Pi). Vale fazer o erro reprovar.

## Etapa 3

Os dois RA-02 ligados conforme a tabela do roteiro, a **80 cm** um do outro.
Depois de religar: `/dev/spidev0.0` e `0.1` presentes, `throttled=0x0` com
os dois rádios.

## Etapa 4

### Passo 0 — 17:43, ✅

`make pi-bench-config`: `configuration valid: radio=sx1278 … profile=NOMINAL`,
2 dBm, LBT por cabeçalho.

### Passo 1 — 17:44, ❌ nenhum dos dois chips responde

`make pi-bench-ping`: o `ttcd` não abre o rádio A
(`radio_open_failed`, `unexpected RegVersion`; log `174357-ping-ttcd.jsonl`).

Diagnóstico pelo SPI, à mão (`spidev` em Python, RESET pulsado e mantido
alto com `gpioset`), lendo RegVersion (0x42, esperado `0x12`), RegOpMode,
RegFrfMsb e RegSyncWord:

| | CE0 (rádio A) | CE1 (rádio B) |
|---|---|---|
| MISO sem pull | `0x00` em todos | `0x00` em todos |
| MISO com pull-up (`pinctrl set 9 pu`) | `0xFF` em todos | `0xFF` em todos |

O MISO **flutua**: nenhum chip o aciona. Do lado da Pi está certo — GPIO9,
10 e 11 em ALT0 (`SPI0_MISO`, `MOSI`, `SCLK`), CE0/CE1 em repouso alto,
RESET (GPIO20, GPIO16) alto. Como os dois falham igual, a suspeita é algo
comum aos dois: os fios compartilhados (MISO, MOSI, SCK), a alimentação, ou
MISO e MOSI trocados no lado do RA-02.

Repetido duas vezes depois de mexer na fiação: igual (`0x00`; `0xFF` com
pull-up). Na terceira, com bytes distintos (`42 A5 5A`) para distinguir um
laço MOSI→MISO de um chip: recebido `00 00 00` (`FF FF FF` com pull-up) nos
dois CE — nem chip, nem eco.

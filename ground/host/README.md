# Ground ADS-B

Este diretório contém o projeto ADS-B da ground, trazido da branch `ADSB-ground` (onde ficava em `ground- adsb/`).

- `ground-aeronaves/`: recepção de NDJSON, estimador por aeroportos/pistas, memória temporal, painel web, simulador e testes.
- `banco-aeronaves/`: importador do cadastro OurAirports. O banco de rotas foi removido.

## Executar

Requer Python 3.10 ou mais recente; o núcleo usa a biblioteca padrão.

```bash
cd ground/host/ground-aeronaves
python3 -m venv .venv
source .venv/bin/activate
python -m unittest discover -s tests -v
```

Os mesmos testes rodam no `make test` (CTest `ground_aeronaves`).

Para simular ou usar o cadastro geográfico, coloque uma cópia do `referencias.db` local em `ground/host/banco-aeronaves/`. Alternativamente, copie os CSVs e o manifesto do cadastro local para `banco-aeronaves/fontes/` e execute `python3 banco-aeronaves/importar_bases.py` a partir desta pasta. O importador verifica os hashes e recusa sobrescrever tabelas existentes. O banco fica ignorado pelo Git.

Com o banco instalado, execute em dois terminais dentro de `ground-aeronaves/`:

```bash
python simular_trajetorias.py
```

```bash
python -m ground.visualize --serve
```

O simulador e o painel usam por padrão `~/adsb/eventos.ndjson`. Para processar um arquivo até o fim: `python -m ground --input /caminho/eventos.ndjson`.

## Do downlink ao estimador

`tracks_to_ndjson.py` converte o log da ground station (um JSON por evento, como o `tools/gs_cli` escreve e como o ESP32 vai mandar pela UART) no NDJSON que o estimador lê: um objeto por registro de aeronave recebido, com o horário refeito no relógio desta máquina — chegada menos tempo no ar menos idade, ADR-0007 — e só os campos que o registro marca como válidos. O teste dele passa cada linha pelo `parser.py` do estimador.

```bash
gs_cli --radio udp ... | python3 tracks_to_ndjson.py | (cd ground-aeronaves && python -m ground --input -)
```

A cadeia inteira sem hardware — captura ou simulação, `adsbd`, `ttcd`, rádio UDP, ground e estimador — é o `make adsb-chain INPUT=...`, descrito em `flight/adsbd/README.md`.

## Avaliação e dados locais

```bash
python validation/gerar_casos_avaliacao.py
python validation/rodar_teste_completo.py
python validation/diagnosticar_estimador.py
```

Os comandos acima exigem o cadastro geográfico. Os casos sintéticos e as respostas esperadas são gerados pelo primeiro comando. `validation/evaluate.py` é uma avaliação histórica que exige o pacote local `validation/baseline-2026-09-24/`; ele não é distribuído aqui. O teste E480F9 é ignorado se o banco ou sua captura local não estiverem presentes.

Não são versionados bancos, capturas NDJSON, dados de referência, estados gerados, pacotes históricos ou ambiente virtual. Para reproduzir exatamente uma avaliação anterior, use os mesmos dados locais. A documentação histórica pode mencionar caminhos do computador original e etapas antigas; este arquivo descreve a organização para GitHub.

As estimativas são heurísticas experimentais. TENTATIVE é uma hipótese provisória; ESTIMATED não confirma o evento nem representa uma probabilidade calibrada.

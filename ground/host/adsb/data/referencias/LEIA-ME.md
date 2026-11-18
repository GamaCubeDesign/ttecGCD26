# Banco de referências aeronáuticas

`referencias.db` contém as bases globais OurAirports baixadas em 22/09/2026 UTC (21/09 no Brasil).

- airports: 86.112 registros, incluindo aeroportos, heliportos, bases de hidroaviões, balonismo e instalações fechadas.
- runways: 48.250 pistas/superfícies de pouso.
- countries: 249 países e territórios.
- regions: 3.987 regiões.
- dataset_sources: fontes, datas, hashes SHA-256 e contagens.

Os nomes de coluna da fonte foram preservados. Valores ausentes são NULL. A chave do aeroporto é o id OurAirports; ICAO e IATA são opcionais. Pistas se relacionam ao aeroporto por airport_ref = airports.id e airport_ident = airports.ident.

14.948 registros de aeroportos não informam elevação. O valor não foi estimado nem substituído por zero. O tipo de instalação fechada nos arquivos baixados é `closed`.

## Consultar

Execute `./abrir-banco.sh` nesta pasta. Para sair: `.quit`.

```sql
SELECT type, COUNT(*) FROM airports GROUP BY type;
SELECT name, icao_code, latitude_deg, longitude_deg, elevation_ft
FROM airports WHERE icao_code = 'SBBR';
SELECT a.name, r.le_ident, r.he_ident, r.length_ft
FROM airports a JOIN runways r ON r.airport_ref = a.id
WHERE a.icao_code = 'SBBR';
```

No Python, conecte com sqlite3.connect(caminho_do_banco). Ative `PRAGMA foreign_keys = ON` em cada conexão que escrever no banco.

## Proveniência e reprodução

CSVs originais: fontes/. Manifesto: fontes/manifesto.json. Verificação: relatorio-importacao.json.
O script importar_bases.py recria as tabelas a partir desses CSVs apenas em banco sem essas tabelas; ele recusa sobrescrevê-las. Não é um atualizador.

Fonte: https://ourairports.com/data/
Dicionário: https://ourairports.com/help/data-dictionary.html
Dados em domínio público, sem garantia de exatidão ou completude pela fonte.

Integridade SQLite e relações entre tabelas verificadas sem erros. O banco de rotas brasileiras e os scripts dedicados de importação/consulta foram removidos em 27/09/2026. `ROTAS-BRASIL.md`, os relatórios e os arquivos fonte DECEA são registros históricos, sem uso pelo sistema atual.

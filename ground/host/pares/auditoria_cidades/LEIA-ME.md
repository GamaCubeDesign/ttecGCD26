# Resultado da auditoria por cidade

Foi realizada uma auditoria automática de **todos os 86.112 registros** da base local, em 247 códigos de países/territórios. O cadastro foi baixado em **22/09/2026**. Há 43.609 grupos municipais com nome informado; destes, **30.891 contêm aeroportos grandes, médios ou pequenos**. Outros 4.746 registros não informam município e foram mantidos individualmente, sem inventar cidades ou reunir locais desconhecidos.

A auditoria cobre o cadastro inteiro. Ela não equivale a verificar manualmente todas as cidades na internet nem a certificar situação operacional atual. A checagem externa foi feita por amostra, com quatro grupos de atendimento regional documentados e uma evidência de serviço regular em Bonito.

## América Latina primeiro

A mesma definição regional configurada para os pares foi aplicada: 26 países/territórios. Resultado:

| Medida | Quantidade |
|---|---:|
| Instalações examinadas | 15.781 |
| Grupos municipais conhecidos com aeroportos de asa fixa | 5.022 |
| Grupos municipais com mais de um aeroporto de asa fixa | 1.426 |
| Aeroportos já presentes nos pares | 403 |
| Aeroportos cadastrados com serviço regular, mas fora dos pares | **89** |
| Desses, localizados no Brasil | **41** |
| Aeroportos médios não incluídos, sem serviço regular confirmado no cadastro | 215 |
| Pequenos aeroportos locais não incluídos, sem serviço regular confirmado no cadastro | 10.450 |
| Aeroportos fora dos pares com todas as pistas cadastradas fechadas | 9 |
| Instalações sem município informado | 516 |

Os grupos são identificados por país + divisão administrativa + município normalizado. São grupos cadastrais, não uma contagem oficial de cidades atendidas. Os 89 aeroportos regulares excluídos correspondem a 88 grupos com município conhecido e uma instalação sem município. São **candidatos à revisão**, não 89 operações comerciais verificadas externamente.

A exclusão sistemática de aeroportos pequenos perde candidatos importantes: **Bonito (SBDB), Juazeiro do Norte (SBJU), Jaguaruna (SBJA), Cabo Frio (SBCB) e Barreiras (SNBR)** aparecem nessa lista na base local. Para Bonito, a [Prefeitura publicou operações regulares de companhias aéreas](https://www.bonito.ms.gov.br/2026/06/26/bonito-ganha-mais-opcoes-de-voos-com-ampliacao-da-operacao-da-latam/), reforçando que porte, sozinho, não basta para a seleção. Essa fonte não verifica todos os demais casos.

## Como São Paulo foi tratado

| Aeroporto | Município cadastrado | Situação nos pares e na auditoria |
|---|---|---|
| SBGR — Guarulhos | São Paulo | Já incluído; mantido individualmente |
| SBSP — Congonhas | São Paulo | Já incluído; mantido individualmente |
| SBKP — Viracopos | Campinas | Já incluído; associação regional a São Paulo documentada separadamente |
| SBMT — Campo de Marte | São Paulo | Fora dos pares; aeroporto médio na lista de alternativas |
| SBJH — Catarina | São Roque | Fora dos pares; médio e próximo, candidato regional a revisão |
| SBJD — Jundiaí | Jundiaí | Fora dos pares; pequeno e próximo, alternativa dependente da aeronave |

Não se escolheu apenas um aeroporto por cidade. Também não se modificou o município físico de Viracopos para São Paulo. A base OurAirports usa “São Paulo” para Guarulhos; essa informação original foi preservada, sem tratá-la como comprovação do município físico.

O grupo documentado de São Paulo contém SBGR, SBSP, SBKP e SBMT. Catarina e Jundiaí constam como vizinhos geográficos, sem afirmação automática de que integram o mesmo sistema aeroportuário oficial.

## Outros casos encontrados

- **Rio de Janeiro:** diferenças de capitalização em “Rio De Janeiro” e “Rio de Janeiro” foram normalizadas; SBGL e SBRJ permanecem distintos. Jacarepaguá (SBJR) aparece como alternativa média fora dos pares.
- **Buenos Aires:** SAEZ (“Buenos Aires (Ezeiza)”, AR-B) e SABE (“Buenos Aires”, AR-C) são associados por grupo de atendimento documentado, sem fundir seus municípios. [Fonte municipal](https://turismo.buenosaires.gob.ar/en/article/getting-buenos-aires).
- **Cidade do México:** MMMX e MMSM têm divisões administrativas diferentes; MMTO está em Toluca. Os três integram o grupo de atendimento registrado. [Fonte do governo mexicano](https://www.gob.mx/aifa/es/articulos/proyecto-198120?idiom=es).
- **Montevidéu:** SUMU está cadastrado em Ciudad de la Costa; sua associação a Montevidéu foi registrada com a [fonte do operador](https://aeropuertodecarrasco.com.uy/). Outros aeroportos próximos ficam como candidatos a revisão.
- **Curitiba e Belo Horizonte:** Bacacheri (SBBI) e Pampulha (SBBH) aparecem como alternativas médias excluídas pela regra atual.

As fontes dos grupos confirmam associações de atendimento; não tornam todos os membros aptos para qualquer aeronave e não confirmam automaticamente serviço regular. A página de Buenos Aires menciona informações históricas de El Palomar; elas não foram usadas como comprovação de operação atual.

## Resultado mundial e qualidade

No mundo inteiro, há **865 aeroportos pequenos com serviço regular cadastrado fora dos pares**, 1.985 alternativas médias e 5.740 grupos municipais conhecidos com múltiplos aeroportos de asa fixa. Foram preservados nomes homônimos em estados diferentes, municípios ausentes e rótulos compostos.

Os dados de pistas foram examinados: quantidade de pistas cadastradas, quantidade com `closed=0`, maior comprimento informado e superfícies. Ausência de informação não significa pista fechada. Há 36 aeroportos fora dos pares com todas as pistas cadastradas fechadas, e um aeroporto já selecionado (VN-0018) com esse conflito, marcado para revisão. Isso é uma inconsistência cadastral a investigar, não confirmação de fechamento atual.

A auditoria registrou **63.055 relações geográficas** de até 100 km entre um aeroporto já selecionado e outro aeroporto de asa fixa do mesmo país. Isso é uma triagem por proximidade, não prova de atendimento da mesma cidade. Localidades sem aeroporto selecionado também foram auditadas integralmente; elas simplesmente não possuem uma âncora para essa triagem. Relações transfronteiriças não são abrangidas por essa heurística.

## O que está pronto e o que não mudou

As tabelas `city_audit_*` foram adicionadas ao banco de pares. Cada instalação tem uma decisão explicada, dados de pista e alertas de qualidade. Os 10.827.390 pares originais e sua prioridade regional foram preservados; a auditoria **não adiciona aeroportos automaticamente aos pares**.

Recomendação resultante: revisar primeiro os 89 regulares excluídos na América Latina; manter aeroportos médios e pequenos locais como alternativas conforme aeronave e trajetória; usar agrupamentos de atendimento para organizar candidatos sem substituir a identidade de cada aeroporto. A ausência de serviço regular não comprova operação exclusivamente privada, militar ou executiva.

## Arquivos e consultas

- `cidades_america_latina.md`: relatório completo dos grupos da região, com todos os aeroportos de asa fixa, sem limitar ao top 10 ou aos exemplos acima.
- `cidades_mundo.json`: todos os 48.355 grupos cadastrais (43.609 com nome e 4.746 registros sem município), ordenados com América Latina primeiro.
- `aeroportos_regulares_fora_dos_pares.json`: os 89 candidatos regionais, incluindo campos de pistas e alertas.
- `resumo.json`: contagens mundiais e regionais.
- `validacao.json`: resultado das verificações do banco completo.
- `verificacoes_externas.json` e `../grupos_atendimento.json`: fontes e escopo das checagens externas por amostra.

Consultar todos os aeroportos de uma cidade:

```sql
SELECT c.country,c.admin_region,c.municipality,a.ident,a.name,a.type,
       a.scheduled_service,a.in_pairs,a.role
FROM city_audit_cities c JOIN city_audit_airports a USING(city_key)
WHERE c.country='BR' AND c.municipality='São Paulo';
```

Consultar a fila regional de revisão:

```sql
SELECT country,admin_region,municipality,scheduled_gap_count,medium_reserve_count,small_local_count
FROM city_audit_cities
ORDER BY in_latam DESC,review_urgency DESC,country,municipality;
```

Reproduzir na raiz do workspace:

```bash
python3 adsb/pares/auditar_cidades.py
python3 -m unittest discover -s adsb/pares -p test_auditar_cidades.py -v
python3 adsb/pares/validar_auditoria_cidades.py
```

O script usa NumPy e a fonte original em modo somente leitura. A atualização das tabelas é transacional e pode ser repetida. Verifica-se o hash da fonte contra o registrado na geração dos pares antes de iniciar.

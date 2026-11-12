# Pares de aeroportos — ADS-B

`pares_aeroportos.db` é um banco SQLite independente. A base original e o estimador atual não são modificados. O estimador integrado está em `../ground-aeronaves`.

## Seleção inicial

Cobertura mundial da base de referências local: todos os `large_airport`, mais os `medium_airport` com `scheduled_service = 'yes'`, com coordenadas válidas. É uma aproximação de relevância baseada no cadastro, não uma classificação oficial de “principal da cidade”. Não se limita a um aeroporto por município. Pequenos aeroportos, heliportos e outras instalações ficam fora desta primeira versão, mesmo que tenham serviço regular; voos para eles não poderão ser identificados por este conjunto inicial.

Cada par ordenado entre aeroportos distintos aparece uma única vez. A→B e B→A são distintos. Não há filtro de companhia, existência de linha comercial, alcance de aeronave ou distância máxima. A base representa possibilidades geométricas, incluindo combinações operacionalmente inviáveis, que o estimador precisará avaliar ou filtrar.

## Estrutura

- `airports`: cópia dos campos essenciais dos aeroportos selecionados, preservando IDs e identificadores da fonte. Essa cópia torna o novo banco autossuficiente.
- `route_pairs`: origem, destino, distância em quilômetros e rumo inicial em graus verdadeiros, no intervalo [0,360). Chave composta por origem e destino e índice adicional pelo destino.
- `metadata`: versão, estado de geração, seleção, contagens, hash SHA-256 da fonte, data de geração e procedência dos conjuntos de dados originais.

A geometria é o arco mais curto de grande círculo, usando esfera de raio 6371,0088 km. Não há uma polilinha armazenada: os extremos em `airports` permitem reconstruir o arco. O rumo inicial não é um rumo constante para toda a rota. Para pontos coincidentes ou antípodas, o rumo indefinido é NULL e deve ser tratado pelo futuro estimador.

As pontuações dinâmicas de cada aeronave não pertencem a este banco. Processos de análise podem abrir conexões independentes em modo somente leitura. Aceitar apenas bancos com `generation_status = complete`.

## Reproduzir

Na raiz deste workspace, use um nome de saída ainda inexistente:

```bash
python3 ground/host/pares/gerar_pares.py \
  --source ground/host/banco-aeronaves/referencias.db \
  --output ground/host/pares/pares_aeroportos.db
```

O gerador nunca sobrescreve um banco existente. Uma execução interrompida deixa `generation_status = building`; gere em outro arquivo antes de usá-lo. Alterações na seleção ou nas coordenadas exigem uma nova geração.

## Consulta por identificador

```sql
SELECT o.ident AS origem, d.ident AS destino,
       p.distance_km, p.initial_bearing_deg
FROM route_pairs p
JOIN airports o ON o.id = p.origin_id
JOIN airports d ON d.id = p.destination_id
WHERE o.ident = 'SBBR' AND d.ident IN ('SBGR','SBGL','SBCT');
```

A geração verifica N×(N−1) registros, integridade SQLite e chaves estrangeiras. O arquivo `validacao.json` registra verificações adicionais da geometria e exemplos desta geração.

## Prioridade regional — América Latina

A migração `prioridade_regional.py` acrescenta tabelas independentes no mesmo banco de pares, sem mudar `airports`, `route_pairs` ou o estimador atual:

- `regional_airports`: associação do código de país/território à região configurada.
- `regional_pairs`: interseção do arco com a região e prioridade de análise. A geometria é simétrica, portanto é calculada e armazenada uma vez por par não direcionado. A consulta recupera os dois sentidos.
- `regional_metadata`: configuração aplicada, hashes da geometria e do código, versões de dependências e data da migração.
- `route_pairs_regional`: visão dos pares direcionais com `origin_in_region`, `destination_in_region`, `arc_intersects_region` e `analysis_priority`.

Prioridade **2**: pelo menos um extremo na região; **1**: nenhum extremo na região, mas arco com interseção; **0**: demais pares. As prioridades são ordinais, não probabilidades nem pesos numéricos da decisão. O bônus de pontuação permanece **desativado**. Nenhum par é eliminado.

`regiao_config.json` define os 20 países AR, BO, BR, CL, CO, CR, CU, DO, EC, SV, GT, HT, HN, MX, NI, PA, PY, PE, UY, VE e seis territórios PR, GF, GP, MQ, MF, BL. É uma definição operacional explícita; não inclui automaticamente todo o Caribe, Belize, Guiana ou Suriname. Pode ser alterada; ampliar a lista exige disponibilizar também os polígonos correspondentes no GeoJSON. Não se classifica a França continental como latino-americana.

A geometria usa [Natural Earth, unidades administrativas 1:50 milhões](https://github.com/nvkelso/natural-earth-vector/tree/v5.1.2/50m_cultural), de domínio público. A origem exata, versão e hash do download estão em `geografia/fonte.json`. O GeoJSON local contém apenas as 26 unidades selecionadas.

**Limites:** verifica-se interseção com polígonos terrestres generalizados, incluindo seus limites; não se trata de espaço aéreo, mar territorial, FIR ou cobertura do receptor. O arco esférico inteiro é aproximado por segmentos de no máximo 50 km, e cada segmento é testado contra os polígonos (não apenas pontos amostrados). Proximidades de costas/fronteiras e ilhas pequenas têm incerteza cartográfica. Cruzamentos do antimeridiano são tratados sem desenhar uma linha falsa através do mapa. Arcos exatamente antípodas, sem trajeto único, têm interseção NULL; sem extremo na região ficam na prioridade normal. O pré-filtro esférico conservador só é habilitado quando os polígonos cabem dentro da região de segurança.

### Reproduzir a migração

Use Python com NumPy e Shapely 2.1.2 (`requirements-regional.txt`). Nesta máquina, Shapely foi instalado localmente em `deps/` para o Python 3.12 do runtime:

```bash
python3 \
  ground/host/pares/prioridade_regional.py
```

A migração é transacional: uma falha restaura a classificação anterior. Pode ser repetida para atualizar a configuração, mas exige espaço temporário para o journal SQLite. Alterar o JSON sozinho não muda a classificação já persistida: execute a migração novamente. Uma base nova criada por `gerar_pares.py` também precisa passar por esta etapa.

### Consumir a ordem de análise

`iter_prioritized(database)` percorre o índice regional em ordem 2→1→0 e emite ambos os sentidos de cada par. Retorna identificadores de origem e destino, interseção e prioridade. Não carrega milhões de linhas em memória, não altera a pontuação e não trunca a lista. O estimador integrado usa o índice regional e agenda análises em processos separados.

Para inspecionar um par:

```sql
SELECT o.ident AS origem, d.ident AS destino,
       p.origin_in_region, p.destination_in_region,
       p.arc_intersects_region, p.analysis_priority
FROM route_pairs_regional p
JOIN airports o ON o.id = p.origin_id
JOIN airports d ON d.id = p.destination_id
WHERE o.ident = 'SBBR' AND d.ident = 'SBGR';
```

Não se deve executar uma ordenação integral da visão a cada mensagem ADS-B. O iterador usa o índice `regional_pairs_priority`, evitando essa ordenação.

Testes:

```bash
python3 \
  -m unittest discover -s adsb/pares -p 'test_prioridade_regional.py' -v
```

## Auditoria mundial por cidade

A auditoria `auditar_cidades.py` analisa todos os aeroportos e demais instalações da fonte original, incluindo os que não entraram nos pares. Os resultados estão nas tabelas `city_audit_*` e no [relatório da auditoria](auditoria_cidades/LEIA-ME.md). O catálogo mundial cobre todos os grupos cadastrais e prioriza a América Latina; o relatório regional detalha todos os aeroportos de asa fixa por município.

Esta camada distingue município cadastrado, grupo de atendimento documentado e mera proximidade geográfica. Ela sinaliza lacunas de seleção e qualidade dos dados sem ampliar automaticamente a lista de pares. As associações regionais verificadas por amostra estão em `grupos_atendimento.json`; não são uma validação externa de todas as cidades.

# Estratégia recuperada do GPT web — 23/09/2026

Fonte: conversa [Inferir origem e destino](https://chatgpt.com/c/6a9ffebc-4fc0-83e9-8632-9f7d2756122b). Histórico textual recuperado em `historico-web-origem-destino.md`. Esta nota consolida a direção da resposta final da conversa, distinguindo propostas de implementação existente. Não executa nem altera os algoritmos.

## Direção mais recente

O núcleo é trajetória temporal + fase do voo + geometria de aeroportos/pistas + memória das hipóteses. O sistema deve operar offline e admitir trajetórias simuladas nunca vistas. Rotas publicadas e padrões aprendidos são evidências opcionais; ausência de correspondência não elimina um aeroporto.

Fluxo proposto:

```text
NDJSON → validação → armazenamento bruto → gerenciamento por ICAO
 → trajetória ordenada → razão vertical + qualidade dos dados
 → fase do voo → candidatos de aeroportos
 → evidências de aeroporto/pista (+ padrões e rotas opcionais)
 → fusão temporal → origem / destino / confiança / UNKNOWN
 → banco / API / interface
```

## Componentes e critérios

1. Preservar todas as observações recebidas, inclusive incompletas. Campos ausentes continuam ausentes; dados derivados têm proveniência e idade. Não preencher o bruto por interpolação.
2. Estimar razão vertical por regressão de altitude contra tempo numa janela quando necessário: `h(t)=β0+β1 t`, `VR=60 β1` para pés e segundos. Comparar com a razão vertical recebida sem sobrescrever o original.
3. Classificar `GROUND`, `CLIMB`, `LEVEL`, `DESCENT`, `APPROACH`, `UNKNOWN` usando qualidade, limiares, histerese e persistência temporal. HMM/fuzzy ficam como evolução possível.
4. Gerar candidatos por busca espacial em referência offline de aeroportos/pistas. Usar elevação, coordenadas de cabeceira, rumo verdadeiro, comprimento e geometria quando disponíveis.
5. Avaliar séries temporais de distância, aproximação/afastamento, altitude relativa, comportamento vertical, velocidade e convergência. Direção instantânea é evidência fraca fora da aproximação final; curvas não devem eliminar automaticamente o destino.
6. Na aproximação, considerar distância lateral ao eixo estendido da pista, diferença entre track e rumo da pista, distância à cabeceira, altitude relativa e movimento em direção à cabeceira. Pesos dependem da fase.
7. Manter hipóteses por aeronave e atualizá-las temporalmente. A conversa propõe inspiração bayesiana, com cálculo em log, mas não define ainda um modelo de verossimilhança calibrado.
8. Origem: privilegiar início observado, subida e afastamento do aeroporto; reduzir atualização no voo nivelado. Destino: privilegiar aproximação, descida e convergência, reforçadas pela pista.
9. Devolver `UNKNOWN`/indeterminado com motivo quando não houver informação suficiente ou houver ambiguidade. Score não equivale a porcentagem de acerto.
10. Validar por etapas: modelo básico, + pistas, + padrões, + rotas. Medir acertos e abstenções. Clustering, DTW e ML são possibilidades posteriores, não dependências do MVP.

## O que muda em relação ao código atual

O código existente apresenta AEROPORTOS e ROTAS separadamente, com regras heurísticas e histórico de hipóteses. A nova direção prioriza fortalecer o primeiro método e criar inferência temporal antes de depender do banco de rotas.

Ainda precisam ser implementados: regressão vertical, avaliação explícita de qualidade, estados com histerese/persistência e APPROACH, uso da geometria das pistas, pesos por fase e fusão temporal de hipóteses. Guardar uma hipótese anterior não é o mesmo que implementar essa fusão.

A base de rotas e o matcher existente podem ser reaproveitados como diagnóstico/evidência auxiliar. Esta sincronização de contexto não os removeu nem modificou. Python continua sendo a escolha inicial mais recente da discussão de linguagem; PostgreSQL/PostGIS aparece como possibilidade de evolução, não como migração realizada do SQLite atual.

## Pontos ainda abertos e cuidados de implementação

Janelas, limiares, pesos, modelo de qualidade, esquecimento, critérios de UNKNOWN e calibração ainda precisam de definição e validação. O início observado pode não incluir a decolagem: subida sozinha não confirma origem.

As evidências não são necessariamente independentes: fase de descida e razão vertical, por exemplo, podem derivar dos mesmos dados. Uma fusão deve evitar contar a mesma observação repetidamente ou acumular certeza artificial em janelas sobrepostas. Esta é uma observação técnica desta revisão, não uma decisão explicitamente fechada na conversa web.

Os artigos e o regulamento foram discutidos na conversa recuperada. Esta sincronização não revalidou as publicações nem o PDF da missão, e não recuperou um relatório separado da Pesquisa aprofundada; recuperou a resposta final que a sintetiza. Exemplos numéricos antigos de confiança não constituem resultados medidos.

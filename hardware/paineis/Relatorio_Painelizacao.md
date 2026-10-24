# Relatório de painelização - PCBS_GCD

**Data da atualização:** 08/10/2026  
**Ferramenta:** KiCad 10.0.6 (`pcbnew` e `kicad-cli`)  
**Referência do fornecedor:** `MA-1.2.00.03R21.pdf`, revisão 21, Circuibras.

## Estado dos arquivos

Os dois arquivos KiCad, os Gerbers X2 e as furações Excellon foram regenerados com as versões revisadas de OBC e EPS EXTRA. **A EPS EXTRA não apresenta mais itens desconectados após a painelização.** Ainda restam sobreposições de courtyard da TTEC e a camada de roteamento das abas exige validação CAM do fabricante. Os detalhes estão em “Pendências de fabricação”.

Após a inclusão manual das logos nas cópias de `topo` em `Painel_1.kicad_pcb` e `Painel_2.kicad_pcb`, os Gerbers X2 e as furações foram reexportados diretamente desses dois arquivos em 08/10/2026. O gerador não foi executado nessa etapa, preservando as edições manuais. **Uma futura execução de `gerar_paineis.py` recriará os painéis a partir das fontes e não reproduzirá essas logos enquanto elas existirem somente nos arquivos dos painéis.**

### Substituição das placas revisadas

As quatro cópias de OBC agora são geradas de `PCB_NOVAS_REVISADAS/PCB_OBC_NOVA/PCB_OBC.kicad_pcb` (SHA-256 `DE7F89A327B28ACDB1CD8AA52EAE004EFACB5E064E51B71F6CD61A2C188012CE`). As cinco cópias de EPS EXTRA agora são geradas de `PCB_NOVAS_REVISADAS/PCB_EPS_EXTRA_NOVA/PCB_EPS_3_EXTRA.kicad_pcb` (SHA-256 `1248AAD4832F18DCF3DE0B5F17B36F967A939DE97AE9FF2F946F6E865FB95BF8`). As versões anteriores nas pastas `OBC/` e `EPS/` não foram alteradas. Ambos os novos contornos estão fechados e conservam as dimensões nominais de 83 × 83 mm, com duas camadas de cobre. O DRC individual das duas revisões encontrou **zero erros e zero itens desconectados**; permanecem avisos de serigrafia e bibliotecas.

**Correção da zona EPS no painel:** a placa recebida tem uma regra personalizada de isolamento de **0,20 mm** em `PCB_EPS_3_EXTRA.kicad_dru`, enquanto a propriedade armazenada na zona indica 0,50 mm. O projeto unificado não incorporava essa regra própria; por isso, o recálculo com 0,50 mm dividia o plano GND. O gerador agora aplica explicitamente a folga efetiva de **0,20 mm** às cópias EPS e recalcula as zonas. Cada cópia fica com uma região contínua em F.Cu e uma em B.Cu, como a placa de origem. O gerador também interrompe a produção se a quantidade de regiões ou a área de cobre divergir mais de 2% da fonte. A área preenchida é cerca de 0,13% menor no painel por causa dos furos de picote acrescentados à borda; as trilhas, pads e demais desenhos elétricos da fonte foram preservados.

### Aparência no editor e no visualizador 3D

As regiões rosadas no editor correspondem às zonas de cobre preenchidas presentes em algumas placas de origem. As placas sem essas regiões aparecem predominantemente com a cor do fundo do editor. No visualizador 3D, o verde representa a máscara de solda sobre as placas; essa diferença de visualização não indica ausência de placa ou erro de cópia. Não foram acrescentados planos de cobre para uniformizar a aparência, pois isso alteraria o projeto elétrico/térmico. As referências de footprint que cresceram com os prefixos de identificação das cópias foram ocultadas da serigrafia; continuam identificadas internamente no arquivo KiCad.

## Motivo de dois painéis

Todas as nove fontes têm duas camadas de cobre e espessura nominal de 1,6 mm. Sete modelos ocupam cerca de 83 × 83 mm cada (`topo`: 82,8 × 82,8 mm); os dois modelos solares medem 198 × 82,8 mm. Os contornos de origem foram conferidos como polígonos fechados pela API de contorno do KiCad, incluindo os arcos dos cantos. A ausência de netlist e componentes elétricos da `topo` foi tratada como característica prevista.

Quatro unidades de cada modelo, mais uma unidade adicional de EPS CIMA, EPS BAIXO, EPS EXTRA e TTEC, somam aproximadamente **351.470,56 mm²** de área nominal retangular de placa, antes de espaçamentos e trilhos. A maior área útil de dupla face da Tabela 12 do manual é **520 × 590 = 306.800 mm²**. Portanto, uma única matriz é impossível por área; dois painéis são o mínimo. Cada painel resultante mede **415,0 × 528,6 mm**, cabendo na área útil intermediária da Circuibras de **445 × 590 mm** (manual, p. 33). A ocupação retangular das placas é 80,11% da área de cada painel, antes de descontar furos e cantos arredondados.

## Quantidades e arranjo

Cada painel contém **20 placas**. A produção tem **cinco cópias** de EPS CIMA, EPS BAIXO, EPS EXTRA e TTEC, e **quatro cópias** de cada um dos demais cinco modelos. Os posicionamentos nominais estão nos arquivos `Painel_1_mapa.json` e `Painel_2_mapa.json`; a visão geral está em `Mapa_Paineis.png`.

| Linha, de cima para baixo | Placas, da esquerda para a direita |
| --- | --- |
| 1 | SOLAR 1, SOLAR 2 |
| 2 | SOLAR 1, SOLAR 2 |
| 3 | AOCS, AOCS, EPS, EPS |
| 4 | OBC, OBC, EPS CIMA, EPS CIMA |
| 5 | EPS BAIXO, EPS BAIXO, topo, topo |
| 6, Painel 1 | EPS CIMA #3, TTEC #1, TTEC #2, EPS BAIXO #3 |
| 6, Painel 2 | EPS EXTRA #5, TTEC #3, TTEC #4, TTEC #5 |

Os modelos mantêm a orientação original. O vão nominal entre placas adjacentes é de 3 mm. Há trilhos externos de 8 mm nas quatro bordas; as linhas de quatro placas são centralizadas no painel.

## Geometria mecânica e arquivos CAM

- O contorno externo fechado do painel e os furos/recortes internos de cada fonte estão em **`Edge.Cuts`** e no Gerber `Painel_N-Edge_Cuts.gm1`.
- O **percurso nominal de fresagem para destacar as placas** está na camada de usuário **`ROTEAMENTO_TABS`** (User.1) e no Gerber `Painel_N-ROTEAMENTO_TABS.gbr`. O percurso tem interrupções de 5 mm nas bordas superior e inferior de cada placa. Essas interrupções formam duas abas por lado: **80 abas por painel**. Não fresar essas lacunas. A linha representa o contorno nominal; a compensação da ferramenta de corte deve ser aplicada pelo CAM do fabricante.
- Cada aba contém cinco furos NPTH de **0,50 mm**, passo **0,80 mm**: **400 furos de picote por painel**, registrados em `Painel_N-NPTH.drl`. Esta combinação segue o método de fresagem com furos de picote descrito no manual, p. 16. O manual não fixa largura, diâmetro ou passo para essas abas; os valores selecionados precisam de aceite CAM.
- Há três furos de fixação **NPTH de 1,90 mm** por painel, como recomenda o manual, p. 36. Seus centros, em milímetros a partir do canto superior esquerdo, são **(4; 4)**, **(411; 4)** e **(4; 524,6)**.
- Há três fiduciais por painel nos centros **(207,5; 4)**, **(207,5; 524,6)** e **(4; 264,3)**. Cada um tem ilha central de cobre de **1,0 mm**, abertura de máscara de **2,0 mm** e anel protetor de cobre de **3,0 mm** de diâmetro externo e **0,5 mm** de largura, seguindo o anexo E do manual, p. 46.
- As zonas de cobre foram recalculadas depois da montagem. Para impedir que planos de GND das fontes se espalhem pelos vãos entre placas, seus limites foram restringidos a uma região 0,30 mm para dentro do contorno de cada placa. A zona AOCS preserva seu traçado particular; a EPS recebe a folga efetiva de 0,20 mm de sua regra personalizada. A origem e a identidade de todas as redes foram mantidas, e os nomes foram prefixados por modelo e cópia para não unir eletricamente instâncias diferentes.

Cada subpasta `Gerber_Painel_N` contém cobre, máscara, serigrafia, pasta, `Edge.Cuts`, `ROTEAMENTO_TABS`, arquivo de trabalho Gerber X2 (`.gbrjob`), furação PTH e NPTH Excellon e relatório de brocas. O Gerber X2 foi conferido pelos atributos de cabeçalho. Os projetos `Painel_N.kicad_pro` acompanham os `.kicad_pcb`. **Enviar `ROTEAMENTO_TABS.gbr` junto com `Edge_Cuts.gm1` e informar explicitamente ao CAM que a primeira é a rota de fresagem com abas.**

## Validação executada

- As nove fontes foram carregadas pelo KiCad; seus contornos externos são fechados. Não foram usados arquivos de recuperação ou backups em lugar dos `.kicad_pcb` principais.
- O gerador verifica 20 instâncias por painel, as quantidades específicas por modelo, ausência de sobreposição das caixas nominais, área dentro de 445 × 590 mm, preservação das aberturas de máscara e continuidade/área do plano GND das cópias EPS. O Painel 1 tem 764 nomes de rede específicos de cópia; o Painel 2, 767.
- Os dois `.kicad_pcb` foram recarregados pelo `kicad-cli` 10.0.6, e os Gerbers e Excellon foram exportados com sucesso. Cada relatório de furação contém **400 furos NPTH de 0,50 mm** e **três furos NPTH de 1,90 mm**.
- As referências longas de componentes introduzidas pela cópia foram ocultadas na serigrafia. Os demais textos e desenhos de origem foram preservados. A auditoria de textos visíveis nas nove placas de origem não encontrou texto fora dos contornos unitários.
- O DRC completo está em `Painel_1_DRC.json` e `Painel_2_DRC.json`; os DRCs das fontes estão em `Verificacao_Origem/`, incluindo `OBC_revisada.json` e `EPS_EXTRA_revisada.json`.
- O DRC final foi repetido depois da inclusão das logos, com `--refill-zones`, e mantém **dois erros, 240 avisos e zero itens desconectados no Painel 1** e **três erros, 247 avisos e zero itens desconectados no Painel 2**. As logos não acrescentaram violações ao DRC. Não há erro de contorno externo malformado nem colisão geométrica entre placas distintas; as pendências estão descritas abaixo.

## Pendências de fabricação

1. **TTEC - courtyard:** há uma sobreposição de courtyard na fonte; ela se repete duas vezes no Painel 1 e três vezes no Painel 2. O DRC a classifica como erro de montagem. Corrigir ou justificar conforme a montagem real.
2. **Avisos de origem:** permanecem avisos de serigrafia e referências de bibliotecas de footprint. O DRC final registra 240 avisos no Painel 1 e 247 no Painel 2; consultar os JSONs para locais exatos. Os arquivos Gerber são gerados a partir das geometrias incorporadas, mas a conferência da serigrafia e dos footprints segue necessária.
3. **Aprovação CAM da separação:** o manual aceita fresagem com furos de picote, mas exige avaliar a rigidez do painel (p. 16) e não define o padrão exato da aba. Confirmar com a Circuibras o diâmetro da fresa, a compensação do percurso `ROTEAMENTO_TABS`, a robustez dos vãos de 3 mm e a aceitação dos furos de 0,50 mm/passo 0,80 mm antes da ordem de fabricação. A documentação também não especifica acabamento superficial, cor de máscara, cor de serigrafia e tolerância dimensional final; completar a especificação técnica recomendada na p. 36.

**Conclusão:** as quantidades, dimensões, separação em dois painéis, trilhos, furos e fiduciais estão materializadas nos arquivos. **Não é possível confirmar o cumprimento integral dos requisitos de fabricação enquanto as pendências acima não forem resolvidas ou aceitas pelo fabricante.**

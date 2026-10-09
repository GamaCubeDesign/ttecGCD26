import pandas as pd
import subprocess
from pathlib import Path
import os

# CONFIGURAÇÕES

CSV_FILE      = "thermalControlData.csv"
TEMPLATE_FILE = "template.tex"
TEX_FILE      = "relatorio.tex"
PDF_FILE      = "relatorio.pdf"

DOC_NUMBER    = "RT-2026-001"

# LER CSV

print("Lendo arquivo CSV...")

df = pd.read_csv(CSV_FILE)
df["timestamp"] = pd.to_datetime(df["timestamp"])

print(df.head())

# METADADOS DE TEMPO

timestamp_inicio = df["timestamp"].iloc[0]
timestamp_fim    = df["timestamp"].iloc[-1]
tempo_total      = timestamp_fim - timestamp_inicio

# Formatos de data para o LaTeX
doc_date      = timestamp_inicio.strftime("%d/%m/%Y")          # 28/04/2026
doc_date_long = timestamp_inicio.strftime(                      # 28 de abril de 2026
    "%-d de %B de %Y"
)
ts_inicio_fmt = timestamp_inicio.strftime("%d/%m/%Y às %Hh%Mmin")
ts_fim_fmt    = timestamp_fim.strftime("%d/%m/%Y às %Hh%Mmin")

# Formata duração como "X minutos" ou "Xh Ymin"
total_segundos = int(tempo_total.total_seconds())
horas   = total_segundos // 3600
minutos = (total_segundos % 3600) // 60
segundos = total_segundos % 60

if horas > 0:
    tempo_total_fmt = f"{horas}h {minutos:02d}min"
else:
    tempo_total_fmt = f"{minutos} minutos e {segundos} segundos" if segundos else f"{minutos} minutos"

# ESTATÍSTICAS

media_interna = df["temp_interna"].mean()
media_externa = df["temp_externa"].mean()

max_interna   = df["temp_interna"].max()
max_externa   = df["temp_externa"].max()

min_interna   = df["temp_interna"].min()
min_externa   = df["temp_externa"].min()

var_interna   = max_interna - min_interna
var_externa   = max_externa - min_externa

total_samples = len(df)

# DADOS PARA PGFPLOTS
# Eixo X em minutos a partir do início

# Tempo em minutos (ponto decimal, pois pgfplots usa notação inglesa)
df["t_min"] = (
    (df["timestamp"] - timestamp_inicio).dt.total_seconds() / 60
)

pgfplot_lines = []
for _, row in df.iterrows():
    t  = f"{row['t_min']:.4f}".replace(",", ".")
    ti = f"{row['temp_interna']:.1f}".replace(",", ".")
    te = f"{row['temp_externa']:.1f}".replace(",", ".")
    pgfplot_lines.append(f"{t},{ti},{te}")

pgfplot_data = "\n".join(pgfplot_lines)

# Limites do gráfico com margem de 1 °C
xmin = 0
xmax = round(df["t_min"].max() + 0.5)
ymin = round(min(min_interna, min_externa) - 1)
ymax = round(max(max_interna, max_externa) + 1)

# TABELA LATEX
# Linhas alternadas com \rowcolor{cinzaclaro}

print("Gerando tabela LaTeX...")

table_rows = []
for i, (_, row) in enumerate(df.iterrows(), start=1):
    ts  = row["timestamp"].strftime("%Y-%m-%d %H:%M:%S")
    ti  = f"{row['temp_interna']:.1f}".replace(".", ",")
    te  = f"{row['temp_externa']:.1f}".replace(".", ",")
    cor = r"\rowcolor{cinzaclaro}" if i % 2 != 0 else ""
    table_rows.append(f"{cor} {i} & {ts} & {ti} & {te} \\\\")

latex_table = "\n".join(table_rows)

# LER TEMPLATE E SUBSTITUIR PLACEHOLDERS

print("Preenchendo template LaTeX...")

with open(TEMPLATE_FILE, "r", encoding="utf-8") as f:
    latex = f.read()

substituicoes = {
    "<<DOC_NUMBER>>":    DOC_NUMBER,
    "<<DOC_DATE>>":      doc_date,
    "<<DOC_DATE_LONG>>": doc_date_long,
    "<<TIMESTAMP_INICIO>>": ts_inicio_fmt,
    "<<TIMESTAMP_FIM>>":    ts_fim_fmt,
    "<<TEMPO_TOTAL>>":   tempo_total_fmt,
    "<<TOTAL_SAMPLES>>": str(total_samples),
    "<<PGFPLOT_DATA>>":  pgfplot_data,
    "<<TABLE_DATA>>":    latex_table,
    "<<MEDIA_INTERNA>>": f"{media_interna:.2f}".replace(".", ","),
    "<<MEDIA_EXTERNA>>": f"{media_externa:.2f}".replace(".", ","),
    "<<MAX_INTERNA>>":   f"{max_interna:.1f}".replace(".", ","),
    "<<MAX_EXTERNA>>":   f"{max_externa:.1f}".replace(".", ","),
    "<<MIN_INTERNA>>":   f"{min_interna:.1f}".replace(".", ","),
    "<<MIN_EXTERNA>>":   f"{min_externa:.1f}".replace(".", ","),
    "<<VAR_INTERNA>>":   f"{var_interna:.1f}".replace(".", ","),
    "<<VAR_EXTERNA>>":   f"{var_externa:.1f}".replace(".", ","),
    "<<XMIN>>":          str(xmin),
    "<<XMAX>>":          str(xmax),
    "<<YMIN>>":          str(ymin),
    "<<YMAX>>":          str(ymax),
}

for placeholder, valor in substituicoes.items():
    latex = latex.replace(placeholder, valor)

# SALVAR ARQUIVO .tex

print(f"Salvando {TEX_FILE}...")

with open(TEX_FILE, "w", encoding="utf-8") as f:
    f.write(latex)

print(f"Arquivo salvo: {TEX_FILE}")

# COMPILAR PDF (2× para sumário correto)


print("Compilando PDF...")

for passagem in range(1, 3):
    print(f"  pdflatex — passagem {passagem}/2...")
    result = subprocess.run(
        ["pdflatex", "-interaction=nonstopmode", TEX_FILE],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print("  Aviso: pdflatex retornou erro. Verifique relatorio.log.")

# FINALIZAÇÃO

if Path(PDF_FILE).exists():
    print(f"\nPDF gerado com sucesso: {PDF_FILE}")
else:
    print("\nErro ao gerar PDF. Verifique relatorio.log para detalhes.")



arquivos_para_deletar = [
    TEX_FILE,                                    # relatorio.tex
    PDF_FILE.replace(".pdf", ".log"),            # relatorio.log
    PDF_FILE.replace(".pdf", ".aux"),            # relatorio.aux
    PDF_FILE.replace(".pdf", ".toc"),            # relatorio.toc
    PDF_FILE.replace(".pdf", ".out"),            # relatorio.out
]

print("\nLimpando arquivos temporários...")
for arquivo in arquivos_para_deletar:
    if Path(arquivo).exists():
        os.remove(arquivo)
        print(f"  Deletado: {arquivo}")

print("Limpeza concluída.")
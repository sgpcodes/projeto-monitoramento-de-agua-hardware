"""Gera um relatorio HTML a partir do historico.csv baixado do ESP32.

Uso:
    python3 tools/gerar_relatorio.py historico.csv
    python3 tools/gerar_relatorio.py historico.csv -o relatorio.html

O relatorio compara:
  - as AMOSTRAS (nomes dados com o comando n: torneira, lagoa, cafe...);
  - os DIAS de medicao (para o estudo de varios dias);
  - a estabilidade do aparelho (variacao das leituras e conferencias do padrao).

So usa a biblioteca padrao do Python. Os graficos usam Chart.js pela internet
(abrir o HTML com internet); as tabelas aparecem mesmo sem internet.

As contas sao as mesmas do firmware (src/relatorio.h):
  luz que passou T = (amostra - escuro) / (padrao - escuro)
  absorbancia A = -log10(T)
  turbidez = 1 - T(590 nm)   ou, com sensor de turbidez, 1 - mV / mV_padrao
  algas    = A680 - A590
  cor      = A415 - A555
  ciano    = A630 - (A590 + A680) / 2   (experimental)
"""

import argparse
import csv
import html
import json
import math
import statistics
import sys
from collections import OrderedDict, defaultdict
from datetime import datetime
from pathlib import Path

CORES = ["f1_415", "f2_445", "f3_480", "f4_515", "f5_555", "f6_590", "f7_630", "f8_680"]
NOMES_CORES = ["Violeta 415", "Azul 445", "Ciano 480", "Verde 515",
               "Verde-am. 555", "Amarelo 590", "Laranja 630", "Vermelho 680"]
TONS_CORES = ["#7b3fc4", "#3a4fd6", "#1fa2d6", "#2db84b", "#97c435", "#f0b400", "#f06a1d", "#d1242a"]
CANAIS = CORES + ["clear", "nir"]

# Mesmos limites de src/relatorio.h (estimativas, nao normas oficiais).
TURBIDEZ_LIMITES = [(0.05, "Limpa"), (0.15, "Levemente turva"), (0.35, "Turva"), (math.inf, "Muito turva")]
ALGAS_LIMITES = [(0.02, "Nao detectadas"), (0.08, "Sinal fraco"), (math.inf, "Presentes")]
COR_LIMITES = [(0.03, "Normal"), (0.10, "Levemente amarelada"), (math.inf, "Cor de cha")]
MV_MINIMO = 500.0

PALETA = ["#0e7c86", "#d1495b", "#edae49", "#3d5a80", "#66a182", "#8e5572", "#6c757d"]


def palavra(valor, limites):
    if valor is None:
        return "-"
    for limite, texto in limites:
        if valor < limite:
            return texto
    return limites[-1][1]


def numero(texto):
    try:
        return float(texto)
    except (TypeError, ValueError):
        return None


def absorbancia(t):
    return -math.log10(max(t, 0.001))


def media(valores):
    valores = [v for v in valores if v is not None]
    return statistics.fmean(valores) if valores else None


def desvio(valores):
    valores = [v for v in valores if v is not None]
    return statistics.stdev(valores) if len(valores) > 1 else None


def fmt(valor, casas=2):
    return "-" if valor is None else f"{valor:.{casas}f}".replace(".", ",")


# ---- Leitura e calculos -----------------------------------------------------------

def ler_csv(caminho):
    with open(caminho, encoding="utf-8", newline="") as f:
        linhas = list(csv.DictReader(f))
    for linha in linhas:
        for canal in CANAIS + ["turbidez_mv", "cv_max", "ganho", "uptime_s"]:
            linha[canal] = numero(linha.get(canal))
        linha["saturado"] = linha.get("saturado") == "1"
        quando = (linha.get("data_hora") or "").strip()
        linha["quando"] = datetime.strptime(quando, "%Y-%m-%d %H:%M:%S") if quando else None
    return linhas


def calcular(linhas):
    """Percorre o historico em ordem, usando sempre o padrao/escuro mais recente."""
    padrao = None
    escuro = None
    amostras = []
    conferencias = []

    for linha in linhas:
        tipo = linha.get("tipo")
        if tipo == "padrao":
            padrao = linha
            continue
        if tipo == "escuro":
            escuro = linha
            continue
        if padrao is None or padrao["ganho"] != linha["ganho"]:
            continue  # sem padrao valido (ou ganho diferente): nao da para comparar

        def luz(canal):
            e = escuro[canal] if escuro and escuro["ganho"] == linha["ganho"] else 0.0
            faixa = (padrao[canal] or 0) - e
            if faixa <= 0 or linha[canal] is None:
                return None
            return min(max((linha[canal] - e) / faixa, 0.0), 1.5)

        t = [luz(c) for c in CORES]
        if tipo == "conferencia":
            validos = [v for v in t if v is not None]
            if validos:
                conferencias.append({"quando": linha["quando"], "media": statistics.fmean(validos) * 100})
            continue
        if tipo != "amostra" or None in t:
            continue

        a = [absorbancia(v) for v in t]
        mv, mv_padrao = linha["turbidez_mv"], padrao["turbidez_mv"]
        if mv and mv_padrao and mv >= MV_MINIMO and mv_padrao >= MV_MINIMO:
            turbidez, fonte = 1 - mv / mv_padrao, "sensor de turbidez"
        else:
            turbidez, fonte = 1 - t[5], "sensor de cor (590 nm)"

        amostras.append({
            "nome": linha.get("amostra") or "sem_nome",
            "quando": linha["quando"],
            "dia": linha["quando"].date().isoformat() if linha["quando"] else "sem data",
            "luz": t,
            "turbidez": turbidez,
            "fonte_turbidez": fonte,
            "algas": a[7] - a[5],
            "cor": a[0] - a[4],
            "ciano": a[6] - (a[5] + a[7]) / 2,
            "cv": linha["cv_max"],
            "saturado": linha["saturado"],
        })
    return amostras, conferencias


def resumir(grupo):
    return {
        "n": len(grupo),
        "turbidez": media([g["turbidez"] for g in grupo]),
        "turbidez_dp": desvio([g["turbidez"] for g in grupo]),
        "algas": media([g["algas"] for g in grupo]),
        "algas_dp": desvio([g["algas"] for g in grupo]),
        "cor": media([g["cor"] for g in grupo]),
        "cor_dp": desvio([g["cor"] for g in grupo]),
        "ciano": media([g["ciano"] for g in grupo]),
        "luz": [media([g["luz"][i] for g in grupo]) for i in range(len(CORES))],
        "cv": media([g["cv"] for g in grupo]),
        "saturadas": sum(1 for g in grupo if g["saturado"]),
        "inicio": min((g["quando"] for g in grupo if g["quando"]), default=None),
        "fim": max((g["quando"] for g in grupo if g["quando"]), default=None),
        "fonte": grupo[0]["fonte_turbidez"],
    }


def agrupar(amostras, chave):
    grupos = OrderedDict()
    for a in amostras:
        grupos.setdefault(a[chave], []).append(a)
    return OrderedDict((k, resumir(v)) for k, v in grupos.items())


# ---- Texto automatico ---------------------------------------------------------------

def frase(resumo):
    partes = [f"água {palavra(resumo['turbidez'], TURBIDEZ_LIMITES).lower()}"]
    algas = palavra(resumo["algas"], ALGAS_LIMITES)
    partes.append({"Nao detectadas": "sem sinal de algas", "Sinal fraco": "com sinal fraco de algas"}
                  .get(algas, "com sinal de algas"))
    cor = palavra(resumo["cor"], COR_LIMITES)
    partes.append({"Normal": "cor normal", "Levemente amarelada": "levemente amarelada"}
                  .get(cor, "cor de chá (matéria orgânica)"))
    return ", ".join(partes).capitalize() + "."


def conclusoes(por_amostra):
    if len(por_amostra) < 2:
        return []
    itens = list(por_amostra.items())

    def extremo(campo):
        validos = [(k, r[campo]) for k, r in itens if r[campo] is not None]
        return max(validos, key=lambda x: x[1]) if validos else None

    textos = []
    for campo, rotulo in [("turbidez", "é a mais turva"), ("algas", "tem mais sinal de algas"),
                          ("cor", "tem mais cor de chá")]:
        e = extremo(campo)
        if e:
            textos.append(f"A amostra <b>{html.escape(e[0])}</b> {rotulo} ({campo} = {fmt(e[1])}).")

    mais_limpa = min(((k, r["turbidez"]) for k, r in itens if r["turbidez"] is not None),
                     key=lambda x: x[1], default=None)
    if mais_limpa:
        textos.append(f"A amostra <b>{html.escape(mais_limpa[0])}</b> é a mais parecida com a água limpa "
                      f"(deixa passar em média {fmt(media(por_amostra[mais_limpa[0]]['luz']) * 100, 0)}% da luz).")
    return textos


# ---- HTML ---------------------------------------------------------------------------

def tabela_resumo(grupos, titulo_coluna):
    linhas = []
    for nome, r in grupos.items():
        linhas.append(
            "<tr>"
            f"<th>{html.escape(nome)}</th><td>{r['n']}</td>"
            f"<td>{fmt(r['turbidez'])} <small>±{fmt(r['turbidez_dp'])}</small><br><span class='tag'>{palavra(r['turbidez'], TURBIDEZ_LIMITES)}</span></td>"
            f"<td>{fmt(r['algas'])} <small>±{fmt(r['algas_dp'])}</small><br><span class='tag'>{palavra(r['algas'], ALGAS_LIMITES)}</span></td>"
            f"<td>{fmt(r['cor'])} <small>±{fmt(r['cor_dp'])}</small><br><span class='tag'>{palavra(r['cor'], COR_LIMITES)}</span></td>"
            f"<td>{fmt(r['ciano'])}</td>"
            f"<td>{fmt(media(r['luz']) * 100, 0)}%</td>"
            f"<td>{fmt(r['cv'], 1)}%</td>"
            f"<td class='frase'>{frase(r)}</td>"
            "</tr>")
    return (f"<div class='rolagem'><table><thead><tr><th>{titulo_coluna}</th><th>Medições</th>"
            "<th>Turbidez</th><th>Algas</th><th>Cor (chá)</th><th>Cianobact.<br><small>(teste)</small></th>"
            "<th>Luz média</th><th>Variação</th><th>Resumo</th></tr></thead><tbody>"
            + "".join(linhas) + "</tbody></table></div>")


def tabela_cores(grupos, titulo_coluna):
    cab = "".join(f"<th><span class='bolinha' style='background:{tom}'></span>{nome}</th>"
                  for nome, tom in zip(NOMES_CORES, TONS_CORES))
    corpo = "".join(
        f"<tr><th>{html.escape(nome)}</th>" + "".join(f"<td>{fmt(v * 100 if v is not None else None, 0)}%</td>" for v in r["luz"]) + "</tr>"
        for nome, r in grupos.items())
    return (f"<div class='rolagem'><table class='numeros'><thead><tr><th>{titulo_coluna}</th>{cab}</tr></thead>"
            f"<tbody>{corpo}</tbody></table></div>")


def dados_espectro(grupos):
    return {
        "labels": NOMES_CORES,
        "datasets": [{"label": nome, "data": [round(v * 100, 1) if v is not None else None for v in r["luz"]],
                      "color": PALETA[i % len(PALETA)]} for i, (nome, r) in enumerate(grupos.items())],
    }


def dados_indices(grupos):
    nomes = list(grupos.keys())
    return {
        "labels": nomes,
        "turbidez": [round(grupos[n]["turbidez"], 4) if grupos[n]["turbidez"] is not None else None for n in nomes],
        "algas": [round(grupos[n]["algas"], 4) if grupos[n]["algas"] is not None else None for n in nomes],
        "cor": [round(grupos[n]["cor"], 4) if grupos[n]["cor"] is not None else None for n in nomes],
    }


def dados_tempo(amostras):
    """Serie no tempo. Com muitos pontos, faz a media por hora para o grafico ficar legivel."""
    com_hora = [a for a in amostras if a["quando"]]
    if not com_hora:
        return None
    por_hora = len(com_hora) > 600
    baldes = OrderedDict()
    for a in com_hora:
        chave = (a["quando"].strftime("%d/%m %Hh") if por_hora else a["quando"].strftime("%d/%m %H:%M"), a["nome"])
        baldes.setdefault(chave, []).append(a)

    rotulos = list(OrderedDict.fromkeys(k[0] for k in baldes))
    nomes = list(OrderedDict.fromkeys(a["nome"] for a in com_hora))
    series = {}
    for campo in ["turbidez", "algas", "cor"]:
        series[campo] = []
        for i, nome in enumerate(nomes):
            valores = {k[0]: media([a[campo] for a in v]) for k, v in baldes.items() if k[1] == nome}
            series[campo].append({"label": nome, "color": PALETA[i % len(PALETA)],
                                  "data": [round(valores[r], 4) if r in valores and valores[r] is not None else None
                                           for r in rotulos]})
    return {"labels": rotulos, "series": series, "por_hora": por_hora}


def gerar_html(caminho_csv, linhas, amostras, conferencias):
    por_amostra = agrupar(amostras, "nome")
    por_dia = agrupar(amostras, "dia")
    tempo = dados_tempo(amostras)
    datas = [a["quando"] for a in amostras if a["quando"]]
    periodo = (f"{min(datas):%d/%m/%Y %H:%M} até {max(datas):%d/%m/%Y %H:%M}" if datas else "sem data")
    sem_padrao = sum(1 for l in linhas if l.get("tipo") == "amostra") - len(amostras)
    saturadas = sum(1 for a in amostras if a["saturado"])

    avisos = []
    if sem_padrao > 0:
        avisos.append(f"{sem_padrao} medições ficaram de fora porque não havia padrão de água limpa "
                      "(ou o ganho era diferente do padrão).")
    if saturadas:
        avisos.append(f"{saturadas} medições tiveram algum canal saturado (luz forte demais).")
    for c in conferencias:
        if abs(c["media"] - 100) > 3:
            quando = f"{c['quando']:%d/%m %H:%M}" if c["quando"] else "sem hora"
            avisos.append(f"Conferência do padrão em {quando}: {fmt(c['media'], 1)}% "
                          "(a lanterna variou mais de 3%).")

    graficos = {
        "espectroAmostras": dados_espectro(por_amostra),
        "indicesAmostras": dados_indices(por_amostra),
        "espectroDias": dados_espectro(por_dia) if len(por_dia) > 1 else None,
        "tempo": tempo,
    }

    secao_dias = ""
    if len(por_dia) > 1:
        secao_dias = f"""
<section>
  <h2>Comparação por dia</h2>
  <p class="nota">Para o estudo de vários dias: cada linha é a média de todas as medições daquele dia.</p>
  {tabela_resumo(por_dia, "Dia")}
  <div class="grafico"><canvas id="espectroDias"></canvas></div>
  {tabela_cores(por_dia, "Dia")}
</section>"""

    secao_tempo = ""
    if tempo:
        aviso_hora = " (média por hora)" if tempo["por_hora"] else ""
        secao_tempo = f"""
<section>
  <h2>Evolução no tempo{aviso_hora}</h2>
  <div class="botoes" id="seletorTempo">
    <button data-campo="turbidez" class="ativo">Turbidez</button>
    <button data-campo="algas">Algas</button>
    <button data-campo="cor">Cor (chá)</button>
  </div>
  <div class="grafico"><canvas id="tempo"></canvas></div>
</section>"""

    lista_conclusoes = "".join(f"<li>{t}</li>" for t in conclusoes(por_amostra))
    lista_avisos = "".join(f"<li>{html.escape(t)}</li>" for t in avisos)

    return f"""<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Relatório da Água</title>
<style>
  :root {{ --fundo:#f5f7f8; --cartao:#fff; --texto:#14323a; --suave:#5c7279; --linha:#dfe6e8; --destaque:#0e5a6b; --aviso:#fff4e0; }}
  @media (prefers-color-scheme: dark) {{
    :root {{ --fundo:#0f1b1f; --cartao:#16262b; --texto:#e4eef0; --suave:#9db3b8; --linha:#2a3d43; --destaque:#5fc4d1; --aviso:#3a2f1a; }}
  }}
  * {{ box-sizing: border-box; }}
  body {{ margin:0; font-family: system-ui, -apple-system, Segoe UI, sans-serif; background:var(--fundo); color:var(--texto); }}
  header {{ background:#0b4552; color:#fff; padding:28px 16px; }}
  header h1 {{ margin:0 0 6px; font-size:1.6rem; }}
  header p {{ margin:0; opacity:.85; }}
  .faixa {{ display:flex; height:6px; }} .faixa span {{ flex:1; }}
  main {{ max-width:1100px; margin:0 auto; padding:16px; }}
  section {{ background:var(--cartao); border:1px solid var(--linha); border-radius:12px; padding:18px; margin:16px 0; }}
  h2 {{ margin:0 0 12px; font-size:1.2rem; color:var(--destaque); }}
  .nota {{ color:var(--suave); font-size:.9rem; margin-top:0; }}
  .rolagem {{ overflow-x:auto; }}
  table {{ border-collapse:collapse; width:100%; font-size:.9rem; margin:8px 0; }}
  th, td {{ padding:8px 10px; border-bottom:1px solid var(--linha); text-align:left; vertical-align:top; }}
  thead th {{ color:var(--suave); font-weight:600; font-size:.8rem; }}
  table.numeros td {{ text-align:right; font-variant-numeric: tabular-nums; }}
  td small {{ color:var(--suave); }}
  .tag {{ display:inline-block; margin-top:3px; padding:1px 7px; border-radius:10px; background:var(--fundo); font-size:.75rem; }}
  .frase {{ min-width:220px; }}
  .bolinha {{ display:inline-block; width:9px; height:9px; border-radius:50%; margin-right:4px; }}
  .grafico {{ position:relative; height:340px; margin:12px 0; }}
  .lado {{ display:grid; grid-template-columns:1fr 1fr; gap:16px; }}
  @media (max-width:760px) {{ .lado {{ grid-template-columns:1fr; }} }}
  .botoes button {{ border:1px solid var(--linha); background:var(--fundo); color:var(--texto); padding:6px 12px; border-radius:16px; cursor:pointer; margin-right:6px; }}
  .botoes button.ativo {{ background:var(--destaque); color:#fff; border-color:var(--destaque); }}
  .avisos {{ background:var(--aviso); border-radius:8px; padding:10px 14px 10px 30px; }}
  .conclusoes li {{ margin:6px 0; }}
  footer {{ color:var(--suave); font-size:.8rem; text-align:center; padding:20px 16px 40px; }}
</style>
</head>
<body>
<header>
  <h1>Relatório da Água</h1>
  <p>{html.escape(periodo)} · {len(amostras)} medições · {len(por_amostra)} amostra(s)</p>
</header>
<div class="faixa">{''.join(f'<span style="background:{t}"></span>' for t in TONS_CORES)}</div>
<main>

<section>
  <h2>Conclusões</h2>
  <ul class="conclusoes">{lista_conclusoes or "<li>Meça pelo menos duas amostras diferentes (comando n) para comparar.</li>"}</ul>
  {f'<ul class="avisos">{lista_avisos}</ul>' if lista_avisos else ''}
</section>

<section>
  <h2>Comparação das amostras</h2>
  <p class="nota">Média de todas as medições de cada amostra. ± = desvio padrão entre as medições.
  Turbidez: 0 = igual à água limpa, 1 = nenhuma luz passa. Fonte da turbidez: {html.escape(next(iter(por_amostra.values()))['fonte']) if por_amostra else '-'}.</p>
  {tabela_resumo(por_amostra, "Amostra")}
  <div class="lado">
    <div><h3>Assinatura da água</h3><p class="nota">% de luz que passou em cada cor. Linha tracejada = água limpa (100%).</p>
      <div class="grafico"><canvas id="espectroAmostras"></canvas></div></div>
    <div><h3>Índices</h3><p class="nota">Quanto maior, mais turva / mais algas / mais cor de chá.</p>
      <div class="grafico"><canvas id="indicesAmostras"></canvas></div></div>
  </div>
  {tabela_cores(por_amostra, "Amostra")}
</section>
{secao_tempo}
{secao_dias}

<section>
  <h2>Como ler este relatório</h2>
  <ul>
    <li><b>Luz que passou:</b> cada medição é comparada com o <i>padrão de água limpa</i> (100%). Quanto menor, mais a água bloqueia aquela cor.</li>
    <li><b>Turbidez</b> (barro, partículas): bloqueia todas as cores por igual.</li>
    <li><b>Algas</b> (clorofila): bloqueiam mais o azul (445) e o vermelho (680).</li>
    <li><b>Cor de chá</b> (matéria orgânica, café): bloqueia mais o violeta (415) e o azul.</li>
    <li><b>Cianobactérias</b>: marca no laranja (630). Índice experimental.</li>
    <li>As palavras (Limpa, Turva…) usam limites estimados para ajudar a leitura; <b>não são normas oficiais</b>.
      Os índices são <b>relativos</b> (comparam amostras e dias entre si), não concentrações de laboratório.</li>
  </ul>
</section>
</main>
<footer>Gerado de {html.escape(Path(caminho_csv).name)} em {datetime.now():%d/%m/%Y %H:%M} · Monitor da Água (ESP32 + AS7341)</footer>

<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js"></script>
<script>
const DADOS = {json.dumps(graficos)};
if (window.Chart) {{
  const escuro = matchMedia('(prefers-color-scheme: dark)').matches;
  Chart.defaults.color = escuro ? '#9db3b8' : '#5c7279';
  Chart.defaults.borderColor = escuro ? '#2a3d43' : '#dfe6e8';
  Chart.defaults.font.family = 'system-ui, -apple-system, Segoe UI, sans-serif';
  const opcoes = (y) => ({{ responsive:true, maintainAspectRatio:false, interaction:{{mode:'index', intersect:false}},
    plugins:{{legend:{{position:'bottom'}}}}, scales:{{y:{{title:{{display:true, text:y}}}}}} }});

  function espectro(id, dados) {{
    if (!dados) return;
    const conjuntos = dados.datasets.map(d => ({{ label:d.label, data:d.data, borderColor:d.color, backgroundColor:d.color,
      tension:.25, pointRadius:4, borderWidth:2.5 }}));
    conjuntos.push({{ label:'Água limpa (100%)', data:dados.labels.map(() => 100), borderColor:'#8a9ba0',
      borderDash:[6,5], pointRadius:0, borderWidth:1.5 }});
    const o = opcoes('% de luz que passou');
    o.scales.y.min = 0;
    new Chart(document.getElementById(id), {{ type:'line', data:{{labels:dados.labels, datasets:conjuntos}}, options:o }});
  }}
  espectro('espectroAmostras', DADOS.espectroAmostras);
  espectro('espectroDias', DADOS.espectroDias);

  const ind = DADOS.indicesAmostras;
  new Chart(document.getElementById('indicesAmostras'), {{ type:'bar',
    data:{{ labels:ind.labels, datasets:[
      {{label:'Turbidez', data:ind.turbidez, backgroundColor:'#8d6e63'}},
      {{label:'Algas', data:ind.algas, backgroundColor:'#43a047'}},
      {{label:'Cor (chá)', data:ind.cor, backgroundColor:'#f0a202'}} ]}},
    options:opcoes('índice') }});

  if (DADOS.tempo) {{
    const t = DADOS.tempo;
    const grafico = new Chart(document.getElementById('tempo'), {{ type:'line',
      data:{{ labels:t.labels, datasets:[] }}, options:Object.assign(opcoes('índice'), {{spanGaps:true}}) }});
    function mostrar(campo) {{
      grafico.data.datasets = t.series[campo].map(s => ({{ label:s.label, data:s.data, borderColor:s.color,
        backgroundColor:s.color, pointRadius: t.labels.length > 120 ? 0 : 3, borderWidth:2, tension:.2 }}));
      grafico.update();
      document.querySelectorAll('#seletorTempo button').forEach(b => b.classList.toggle('ativo', b.dataset.campo === campo));
    }}
    document.querySelectorAll('#seletorTempo button').forEach(b => b.addEventListener('click', () => mostrar(b.dataset.campo)));
    mostrar('turbidez');
  }}
}}
</script>
</body>
</html>"""


def main():
    parser = argparse.ArgumentParser(description="Gera o relatorio HTML a partir do historico.csv do ESP32")
    parser.add_argument("csv", help="arquivo historico.csv baixado da placa")
    parser.add_argument("-o", "--saida", help="arquivo HTML de saida (padrao: relatorio_<data>.html)")
    args = parser.parse_args()

    linhas = ler_csv(args.csv)
    amostras, conferencias = calcular(linhas)
    if not amostras:
        print("Nenhuma medicao de amostra com padrao de agua limpa no arquivo.")
        print("Confira se a placa salvou um padrao (mensagem 'PADRAO DE AGUA LIMPA SALVO').")
        sys.exit(1)

    saida = Path(args.saida or f"relatorio_{datetime.now():%Y%m%d_%H%M}.html")
    saida.write_text(gerar_html(args.csv, linhas, amostras, conferencias), encoding="utf-8")

    print(f"Relatorio gerado: {saida.resolve()}")
    for nome, r in agrupar(amostras, "nome").items():
        print(f"  {nome:<14} {r['n']:>5} medicoes  turbidez {fmt(r['turbidez'])}  algas {fmt(r['algas'])}  cor {fmt(r['cor'])}")


if __name__ == "__main__":
    main()

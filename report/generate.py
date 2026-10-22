#!/usr/bin/env python3
"""Regenerate the unified LaTeX report from its Markdown source; no dependencies."""
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parent
DIAGRAMS = {
 'project_report': [
     ('schedule-pipeline', 'Any generator can produce GOAL; conversion runs once per schedule, and the binary is simulated as often as needed.'),
     ('local-dependencies', 'Solid arrows: local completion dependencies. Dashed arrows: matching messages between ranks. Roots have a thick border.'),
     ('simulation-loop', 'Events are processed in simulated time order.'),
     ('legacy-file-sections', 'Legacy file order: roots precede the fixed-size records and dependency appendix.'),
     ('legacy-record', 'The legacy node record.'),
     ('design-overview', 'Pass 1 determines the schema for the whole input. Pass 2 alternates parsing and writing for each rank. Numbers refer to subsections.'),
     ('conversion-overview', 'Infer the layout first, then use it to pack every rank.'),
     ('file-sections', 'Blue: the schema added to the header. Grey: existing components, with records and indices stored at inferred widths.'),
     ('header-details', 'Stored fields in file order; cell widths are illustrative. The name bytes occur only for custom fields.'),
     ('schema-agreement', 'Both adapters inherit the same interface, specialized for their node type. Each simulator owns a separate adapter and reader instance.'),
     ('node-read-call', 'Construct once, then read nodes on demand.'),
     ('runtime-specialization', 'Proposed integration: specialize decoding beneath the adapter while retaining the shared reader and generic fallback.'),
     ('legacy-gpu-record', 'How ATLAHS fills the legacy record; nothing in the file records this.'),
     ('packed-gpu-record', 'Four-GPU example: 3 bytes per node instead of 39, including all eight custom fields.'),
 ]}
ESC = {'\\':r'\textbackslash{}','&':r'\&','%':r'\%','$':r'\$','#':r'\#','_':r'\_','{':r'\{','}':r'\}','~':r'\textasciitilde{}','^':r'\textasciicircum{}',
       '→':r'\(\rightarrow\)','−':r'\(-\)','×':r'\(\times\)','—':'---','–':'--','’':"'",'“':'``','”':"''",'…':r'\ldots{}'}
def escape(s): return ''.join(ESC.get(c,c) for c in s)
def code(s):
    # Permit line breaks in identifiers/paths, including long unpunctuated names.
    parts=[];n=0
    for c in s:
        parts.append(escape(c));n+=1
        if c in '/_:.=-' or n>=12: parts.append(r'\allowbreak{}');n=0
    return r'\texttt{'+''.join(parts)+'}'
def slug(s):
    return re.sub(r'[^\w -]','',s.replace('`','')).lower().replace(' ','-')
def inline(s):
    out=[];i=0
    while i<len(s):
        if s[i]=='`':
            j=s.find('`',i+1)
            if j!=-1: out.append(code(s[i+1:j]));i=j+1;continue
        if s.startswith('**',i):
            j=s.find('**',i+2)
            if j!=-1:out.append(r'\textbf{'+inline(s[i+2:j])+'}');i=j+2;continue
        if s[i]=='[':
            m=re.match(r'\[([^\]]+)\]\(([^)]+)\)',s[i:])
            if m:
                label,target=m.groups();label=inline(label)
                if target.startswith('#'): out.append(r'\hyperlink{'+target[1:]+'}{'+label+'}')
                else:
                    target=re.sub(r'^project_report\.md', 'project_report.pdf', target)
                    if not re.match(r'\w+://',target) and not target.startswith('project_report.pdf'):target='../'+target
                    out.append(r'\href{\detokenize{'+target+'}}{'+label+'}')
                i+=m.end();continue
        out.append(escape(s[i]));i+=1
    return ''.join(out)
def convert(stem):
    lines=(ROOT.parent/(stem+'.md')).read_text().splitlines();o=[];i=0;diagram=0
    while i<len(lines):
        line=lines[i]
        if line.startswith('# '):
            title=line[2:];o.extend([r'\documentclass[10pt,a4paper]{article}',r'\usepackage{plan-style}',r'\title{'+inline(title)+'}',r'\author{}',r'\date{}',r'\begin{document}',r'\maketitle',r'\setcounter{tocdepth}{2}',r'\tableofcontents',r'\clearpage']);i+=1;continue
        if line.startswith('```'):
            lang=line[3:];i+=1;block=[]
            while i<len(lines) and not lines[i].startswith('```'):block.append(lines[i]);i+=1
            i+=1
            if lang=='mermaid':
                name,caption=DIAGRAMS[stem][diagram];diagram+=1
                # All drawings remain vector TikZ; constrain width only where needed.
                width = {'local-dependencies': r'0.68\linewidth',
                         'simulation-loop': r'0.85\linewidth'}.get(name, r'\linewidth')
                o.extend([r'\begin{center}',r'\resizebox{'+width+r'}{!}{\input{diagrams/'+name+'.tex}}',r'\par\nopagebreak\small\itshape '+inline(caption),r'\end{center}'])
            else:o.extend([r'\begin{lstlisting}',*block,r'\end{lstlisting}'])
            continue
        if line.startswith('##'):
            m=re.match(r'(#+) (.*)',line);level,title=m.groups();cmd={2:'section',3:'subsection',4:'subsubsection'}.get(len(level),'paragraph')
            o.extend([r'\hypertarget{'+slug(title)+'}{}', '\\'+cmd+'{'+inline(title)+'}']);i+=1;continue
        if line.startswith('|'):
            rows=[]
            while i<len(lines) and lines[i].startswith('|'):
                cells=[x.strip() for x in lines[i].strip().strip('|').split('|')]
                if not all(re.fullmatch(r':?-+:?',x) for x in cells):rows.append(cells)
                i+=1
            n=len(rows[0]);widths={2:[.28,.72],3:[.22,.37,.41],4:[.30,.10,.47,.13]}.get(n,[1/n]*n)
            if n==3 and rows[0][0]=='Goal': widths=[.30,.58,.12]
            if n==3 and rows[0][0]=='Part': widths=[.17,.21,.62]
            if n==3 and rows[0][0]=='Step': widths=[.13,.435,.435]
            if n==3 and rows[0][0]=='Approach': widths=[.19,.39,.42]
            if n==4:
                if rows[0][0]=='Approach':
                    widths=[.22,.12,.22,.44]
                elif rows[0][0]=='Record field':
                    widths=[.15,.12,.10,.63]
                elif rows[0][0]=='Information':
                    widths=[.16,.12,.38,.34]
                elif rows[0][0]=='Trace':
                    widths=[.38,.10,.24,.28]
                elif rows[0][0]=='Rank 0':
                    widths=[.40,.10,.40,.10]
            columns=''.join('P{'+str(round(w,3))+r'\dimexpr\linewidth-'+str(2*n)+r'\tabcolsep\relax}' for w in widths)
            if rows[0]==['Approach', 'How it works', 'Main tradeoff']:
                # Keep the compact comparison together on one page.
                o.extend([r'\par\medskip\noindent\begin{minipage}{\linewidth}',
                          r'\small\renewcommand{\arraystretch}{1.15}',
                          r'\begin{tabular}{'+columns+'}', r'\toprule',
                          ' & '.join(r'\textbf{'+inline(c)+'}' for c in rows[0])+r' \\',
                          r'\midrule'])
                for row in rows[1:]: o.append(' & '.join(inline(c) for c in row)+r' \\')
                o.extend([r'\bottomrule', r'\end{tabular}', r'\end{minipage}\par\medskip'])
                continue
            o.extend([r'{\small',r'\begin{longtable}{'+columns+'}',r'\toprule'])
            head=' & '.join(r'\textbf{'+inline(c)+'}' for c in rows[0])+r' \\'
            o.extend([head,r'\midrule',r'\endfirsthead',r'\toprule',head,r'\midrule',r'\endhead',r'\bottomrule',r'\endfoot'])
            for row in rows[1:]:o.append(' & '.join(inline(c) for c in row)+r' \\')
            o.extend([r'\end{longtable}','}']);continue
        if re.match(r'(- |\d+\. )',line):
            numbered=bool(re.match(r'\d+\. ',line));env='enumerate' if numbered else 'itemize';o.append('\\begin{'+env+'}')
            while i<len(lines) and re.match(r'\d+\. ' if numbered else r'- ',lines[i]):
                o.append(r'\item '+inline(re.sub(r'^(\d+\. |- )','',lines[i])));i+=1
            o.append('\\end{'+env+'}');continue
        o.append(inline(line));i+=1
    o.append(r'\end{document}')
    assert diagram==len(DIAGRAMS[stem])
    (ROOT/(stem+'.tex')).write_text('% Generated by report/generate.py from ../'+stem+'.md; edit Markdown to preserve synchronization.\n'+'\n'.join(o)+'\n')
for stem in DIAGRAMS:convert(stem)

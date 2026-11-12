"""Protótipo de exaustor centrífugo NB3: CAD paramétrico em milímetros.

Geometria verificada por B-Rep e malha; desempenho e resistência não certificados.
O eixo Z aponta do motor para a sucção. Visto de +Z, o rotor gira no sentido horário.
"""
from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path

import cadquery as cq
import numpy as np
from scipy.optimize import brentq

BASE = Path(__file__).resolve().parents[1]
PADRAO = {
    "unidade": "mm",
    "estado": "PROTOTIPO — conferir encaixes, balancear e ensaiar antes de operar",
    "motor": {"diametro_frontal":77.09,"diametro_ressalto":49.95,"altura_ressalto":2.5,
              "diametro_eixo":9.08,"medida_plano_eixo":8.08,"eixo_ate_ponta":24.0,
              "circulo_furacao":64.5,"rosca_presumida":"M4 — confirmar no gabarito",
              "rpm":1650,"potencia_placa_W":37,"tensao_placa":"110/220 V, 60 Hz",
              "sentido_visto_pela_ponta":"horario"},
    "rotor":{"diametro":240.0,"disco":3.5,"altura_pas":20.0,"numero_pas":12,
             "espessura_pas":3.2,"espessura_pe_pas":5.6,"altura_pe_pas":2.0,
             "raio_inicio_pas":30.0,"raio_fim_pas":116.5,"angulo_saida_graus":60.0,
             "diametro_cubo":28.0,"altura_cubo":17.0,"folga_diametral_eixo":0.30,
             "centro_porcas_cubo":9.4,"altura_parafusos_cubo":9.0},
    "carcaca":{"placa":4.0,"reforco_motor":4.0,"tampa":4.0,"parede":4.0,
               "folga_traseira":2.0,"folga_frontal":1.5,"folga_radial_minima":3.5,
               "arredondamento_lingua":2.0,"crescimento_voluta":30.0,
               "inicio_espiral_graus":30.0,"fim_descarga_y":150.0,
               "flange_externa":10.0,"espessura_flange":3.0,
               "raio_orelhas":6.5,"afastamento_furos_corpo":8.5,
               "canal_vedacao_largura":2.8,"canal_vedacao_profundidade":1.5,
               "diametro_furos_m4":4.5,"encaixe_ressalto_folga":0.5,
               "encaixe_ressalto_fundo":0.4},
    "succao":{"tubo_diametro_externo":14.0,"furo_encaixe":14.2,"diametro_olho":60.0,
              "altura_cone":28.0,"altura_soquete":22.0,"diametro_soquete_externo":24.0,
              "diametro_fundo_canais_oring":17.4,"largura_canais_oring":2.6,
              "oring_referencia":"14 x 2 mm, 2 unidades"},
    "impressao":{"material":"PETG","mesa_x":350,"mesa_y":350,"mesa_z":350},
    "processo":{"pressao_sala_min_Pa":-80,"pressao_sala_max_Pa":-60,
                "comprimento_tubo_mm":350,"diametro_interno_tubo":"NAO INFORMADO",
                "garantia_de_pressao":False,"curva_medida":False}
}


def cilindro(raio,altura,z=0,x=0,y=0):
    return cq.Workplane('XY').center(x,y).circle(raio).extrude(altura).translate((0,0,z)).val()


def prisma(fio,altura,z=0):
    return cq.Workplane('XY').add(fio).toPending().extrude(altura).val().translate((0,0,z))


def caixa(dx,dy,dz,x=0,y=0,z=0):
    return cq.Workplane('XY').box(dx,dy,dz).translate((x,y,z)).val()


def unir(formas):
    formas=list(formas)
    return formas[0].fuse(*formas[1:]).clean() if len(formas)>1 else formas[0]


def fio_d(diametro,medida_plano,folga_diametral):
    r=diametro/2+folga_diametral/2
    x=medida_plano-diametro/2+folga_diametral/2
    y=math.sqrt(r*r-x*x)
    return cq.Workplane('XY').moveTo(x,y).threePointArc((-r,0),(x,-y)).close().val()


def porca_z(af,altura,x,y,z):
    r=af/math.sqrt(3)
    pontos=[(x+r*math.cos(math.radians(30+i*60)),y+r*math.sin(math.radians(30+i*60))) for i in range(6)]
    return cq.Workplane('XY').polyline(pontos).close().extrude(altura).translate((0,0,z)).val()


def gerar_perfil(p):
    r=p['rotor'];c=p['carcaca']
    raio0=r['diametro']/2+c['folga_radial_minima']
    a0=math.radians(c['inicio_espiral_graus'])
    derivada=c['crescimento_voluta']/(2*math.pi-a0)
    esquerda=raio0*math.cos(a0)
    raio_nariz=c['arredondamento_lingua']
    def ponto_e_normal(a):
        raio=raio0+derivada*(a-a0)
        x=raio*math.cos(a);y=raio*math.sin(a)
        dx=derivada*math.cos(a)-raio*math.sin(a)
        dy=derivada*math.sin(a)+raio*math.cos(a)
        norma=math.hypot(dx,dy)
        return x,y,dy/norma,-dx/norma,dx,dy
    tangencia=brentq(lambda a:ponto_e_normal(a)[0]+raio_nariz*ponto_e_normal(a)[2]-(esquerda-raio_nariz),a0,a0+.7)
    xq,yq,nx,ny,dx,dy=ponto_e_normal(tangencia)
    centro_nariz=(esquerda-raio_nariz,yq+raio_nariz*ny)
    pontos=[]
    for t in np.linspace(0,1,161):
        a=tangencia+(2*math.pi-tangencia)*t
        xx,yy,*_=ponto_e_normal(a)
        pontos.append((xx,yy))
    final_y=c['fim_descarga_y']
    *_,dx_f,dy_f=ponto_e_normal(2*math.pi)
    fio=(cq.Workplane('XY').moveTo(*pontos[0]).spline(pontos[1:],includeCurrent=True,tangents=[(dx,dy),(dx_f,dy_f)])
         .lineTo(pontos[-1][0],final_y).lineTo(esquerda,final_y).lineTo(esquerda,centro_nariz[1])
         .threePointArc((centro_nariz[0],centro_nariz[1]-raio_nariz),pontos[0]).close().val())
    # O perfil base é anti-horário; o espelhamento gera a voluta para giro horário.
    fio=fio.mirror('XZ')
    furos=[]
    for grau in [45,90,135,180,225,270,315]:
        a=math.radians(grau)
        t=(a-a0)/(2*math.pi-a0)
        rr=raio0+c['crescimento_voluta']*t+c['afastamento_furos_corpo']
        furos.append((rr*math.cos(a),-rr*math.sin(a)))
    direita=pontos[-1][0]
    furos.extend([(esquerda-8,-final_y+10),(direita+8,-final_y+10)])
    return fio,furos,esquerda,direita,pontos


_CACHE_DESLOCAMENTOS={}


def deslocar_fio(fio,distancia):
    if distancia==0:return fio
    chave=(id(fio),float(distancia))
    if chave in _CACHE_DESLOCAMENTOS:return _CACHE_DESLOCAMENTOS[chave]
    fios=fio.offset2D(distancia,'arc')
    assert len(fios)==1,(distancia,len(fios))
    _CACHE_DESLOCAMENTOS[chave]=fios[0]
    return fios[0]


def construir_placa(p,fio,furos):
    c=p['carcaca'];m=p['motor'];t=c['placa'];ref=c['reforco_motor'];r_orelha=c['raio_orelhas']
    cheio=prisma(deslocar_fio(fio,c['flange_externa']),t)
    reforcos=[cilindro(41,ref,-ref)]
    for x,y in furos:
        reforcos.extend([cilindro(r_orelha,t,0,x,y),cilindro(r_orelha,3,-3,x,y)])
    placa=unir([cheio,*reforcos])
    cortes=[cilindro(6,t+ref+2,-ref-1),
            cilindro((m['diametro_ressalto']+c['encaixe_ressalto_folga'])/2,
                     m['altura_ressalto']+c['encaixe_ressalto_fundo']+.05,-ref-.05)]
    # O-ring estático 54 x 2 no apoio frontal do motor.
    cortes.append(cilindro(29.4,1.55,-ref-.05).cut(cilindro(26.6,1.7,-ref-.1)))
    for a in range(0,360,90):
        x=m['circulo_furacao']/2*math.cos(math.radians(a));y=m['circulo_furacao']/2*math.sin(math.radians(a))
        cortes.extend([cilindro(c['diametro_furos_m4']/2,t+ref+2,-ref-1,x,y),
                       cilindro(5.25,5.65,t-5.6,x,y)])
    for x,y in furos:
        cortes.extend([cilindro(c['diametro_furos_m4']/2,t+5,-4,x,y),porca_z(7.3,5.35,x,y,-3.05)])
    canal=prisma(deslocar_fio(fio,3.4),1.55,t-1.5).cut(prisma(deslocar_fio(fio,.6),1.7,t-1.6))
    cortes.append(canal)
    return placa.cut(*cortes).clean()


def construir_voluta(p,fio,furos,esquerda,direita):
    c=p['carcaca'];r=p['rotor'];h=c['folga_traseira']+r['disco']+r['altura_pas']+c['folga_frontal']
    parede=prisma(deslocar_fio(fio,c['parede']),h)
    base=prisma(deslocar_fio(fio,c['flange_externa']),c['espessura_flange'])
    delta=c['flange_externa']-c['parede'];z0=h-c['espessura_flange']-delta
    # Degraus de 1 mm evitam superfícies de loft mal condicionadas e limitam o balanço de impressão.
    passos=round(delta)
    alto=[prisma(deslocar_fio(fio,c['parede']+delta*i/passos),delta/passos,
                 z0+delta*(i-1)/passos) for i in range(1,passos+1)]
    topo=prisma(deslocar_fio(fio,c['flange_externa']),c['espessura_flange'],h-c['espessura_flange'])
    corpo=unir([parede,base,*alto,topo,*[cilindro(c['raio_orelhas'],h,0,x,y) for x,y in furos]])
    corpo=corpo.cut(prisma(fio,h+2,-1))
    cortes=[cilindro(c['diametro_furos_m4']/2,h+2,-1,x,y) for x,y in furos]
    # Grade de descarga integrada: pontes curtas, barras e moldura unidas ao corpo.
    largura=direita-esquerda;n=6;abertura=5.0;margem=2.5
    passo=(largura-2*margem-abertura)/(n-1)
    for i in range(n):
        x=esquerda+margem+abertura/2+i*passo
        cortes.append(caixa(abertura,15,h-5,x,-c['fim_descarga_y']-3,h/2))
    corpo=corpo.cut(*cortes).clean()
    return corpo,h


def construir_tampa(p,fio,furos):
    c=p['carcaca'];s=p['succao'];t=c['tampa']
    tampa=unir([prisma(deslocar_fio(fio,c['flange_externa']),t),
                *[cilindro(c['raio_orelhas'],t,0,x,y) for x,y in furos]])
    rbase=s['diametro_olho']/2+2;rtopo=s['diametro_soquete_externo']/2;hc=s['altura_cone'];hs=s['altura_soquete']
    cone=cq.Solid.makeCone(rbase,rtopo,hc,cq.Vector(0,0,t),cq.Vector(0,0,1))
    tubo=cilindro(rtopo,hs,t+hc)
    tampa=unir([tampa,cone,tubo])
    cortes=[cilindro(s['diametro_olho']/2,t+.1,-.05),
            cq.Solid.makeCone(s['diametro_olho']/2,s['furo_encaixe']/2,hc+.02,cq.Vector(0,0,t-.01),cq.Vector(0,0,1)),
            cilindro(s['furo_encaixe']/2,hs+1,t+hc-.01)]
    topo=t+hc+hs
    for profundidade in [6,14]:
        cortes.append(cilindro(s['diametro_fundo_canais_oring']/2,s['largura_canais_oring'],
                               topo-profundidade-s['largura_canais_oring']/2))
    cortes.append(cq.Solid.makeCone(s['furo_encaixe']/2,s['furo_encaixe']/2+.6,.61,
                                   cq.Vector(0,0,topo-.6),cq.Vector(0,0,1)))
    cortes.extend(cilindro(c['diametro_furos_m4']/2,t+2,-1,x,y) for x,y in furos)
    canal=prisma(deslocar_fio(fio,3.4),1.55,-.05).cut(prisma(deslocar_fio(fio,.6),1.7,-.1))
    cortes.append(canal)
    return tampa.cut(*cortes).clean()


def perfil_pa(r1,r2,espessura,angulo_inicial,beta):
    centro=[];tangentes=[];k=1/math.tan(math.radians(beta))
    for r in np.linspace(r1,r2,36):
        a=angulo_inicial+k*math.log(r/r1)  # Pá curvada para trás para rotação horária.
        centro.append(np.array([r*math.cos(a),r*math.sin(a)]))
        tang=np.array([math.cos(a)-k*math.sin(a),math.sin(a)+k*math.cos(a)])
        tangentes.append(tang/np.linalg.norm(tang))
    esquerda=[];direita=[]
    for v,tg in zip(centro,tangentes):
        normal=np.array([-tg[1],tg[0]])
        esquerda.append(tuple(v+normal*espessura/2));direita.append(tuple(v-normal*espessura/2))
    ponta=tuple(centro[-1]+tangentes[-1]*espessura/2)
    pe=tuple(centro[0]-tangentes[0]*espessura/2)
    return (cq.Workplane('XY').moveTo(*esquerda[0]).spline(esquerda[1:],includeCurrent=True)
            .threePointArc(ponta,direita[-1]).spline(list(reversed(direita[:-1])),includeCurrent=True)
            .threePointArc(pe,esquerda[0]).close().val())


def construir_rotor(p):
    r=p['rotor'];m=p['motor'];t=r['disco']
    disco=cilindro(r['diametro']/2,t)
    cubo=cq.Workplane('XY').circle(r['diametro_cubo']/2).extrude(r['altura_cubo']).edges('>Z').chamfer(.8).val()
    partes=[disco,cubo]
    for i in range(r['numero_pas']):
        a=i*2*math.pi/r['numero_pas']
        fio=perfil_pa(r['raio_inicio_pas'],r['raio_fim_pas'],r['espessura_pas'],a,r['angulo_saida_graus'])
        pe=perfil_pa(r['raio_inicio_pas'],r['raio_fim_pas'],r['espessura_pe_pas'],a,r['angulo_saida_graus'])
        partes.extend([prisma(fio,r['altura_pas'],t),cq.Solid.makeLoft([pe.translate((0,0,t)),
                       fio.translate((0,0,t+r['altura_pe_pas']))],True)])
    rotor=unir(partes)
    cortes=[prisma(fio_d(m['diametro_eixo'],m['medida_plano_eixo'],r['folga_diametral_eixo']),r['altura_cubo']+2,-1)]
    entrada=cq.Solid.makeLoft([
        fio_d(m['diametro_eixo'],m['medida_plano_eixo'],r['folga_diametral_eixo']+.8).translate((0,0,-.05)),
        fio_d(m['diametro_eixo'],m['medida_plano_eixo'],r['folga_diametral_eixo']).translate((0,0,.5))],True)
    cortes.append(entrada)
    z=r['altura_parafusos_cubo'];x=r['centro_porcas_cubo'];af=7.3;th=3.6
    furo=cq.Workplane(cq.Plane(origin=(-16,0,z),xDir=(0,1,0),normal=(1,0,0))).circle(2.2).extrude(32).val()
    cortes.append(furo)
    hexagono=[(af/math.sqrt(3)*math.cos(math.radians(90+i*60)),af/math.sqrt(3)*math.sin(math.radians(90+i*60))) for i in range(6)]
    plano=cq.Plane(origin=(x-th/2,0,z),xDir=(0,1,0),normal=(1,0,0))
    porca=cq.Workplane(plano).polyline(hexagono).close().extrude(th).val()
    acesso=caixa(th,af+.1,r['altura_cubo']-z+.2,x,0,(r['altura_cubo']+z)/2+.1)
    for corte in [porca,acesso]:cortes.extend([corte,corte.mirror('YZ')])
    return rotor.cut(*cortes).clean()


def construir_gabarito(p):
    m=p['motor'];esp=6.0
    g=cilindro(41,esp)
    cortes=[cilindro(6,esp+2,-1),cilindro((m['diametro_ressalto']+.5)/2,m['altura_ressalto']+.4+.05,-.05)]
    for a in range(0,360,90):
        x=m['circulo_furacao']/2*math.cos(math.radians(a));y=m['circulo_furacao']/2*math.sin(math.radians(a))
        cortes.append(cilindro(2.25,esp+2,-1,x,y))
    return g.cut(*cortes).clean()


def verificar(forma,nome):
    assert forma.isValid(),('BRep inválido',nome)
    assert len(forma.Solids())==1,('Deve ser um único sólido',nome,len(forma.Solids()))
    assert forma.Volume()>0,nome


def volume_por_triangulos(arquivo):
    # A integração padrão do kernel é sensível aos recortes B-spline da voluta.
    # Usar o volume da superfície triangulada fechada, no mesmo refinamento do STL.
    tipo=np.dtype([('normal','<f4',(3,)),('v','<f4',(3,3)),('atributo','<u2')])
    triangulos=np.fromfile(arquivo,dtype=tipo,offset=84)['v'].astype(float)
    a=triangulos[:,0];b=triangulos[:,1];c=triangulos[:,2]
    return abs(np.einsum('ij,ij->i',a,np.cross(b,c)).sum()/6)


def principal():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parametros',type=Path,default=BASE/'cad/parametros.json')
    parser.add_argument('--saida',type=Path,default=BASE/'saida')
    parser.add_argument('--reusar-modelo',action='store_true',help='Reutiliza B-Rep local para revisão de exportação; não usar após mudar parâmetros.')
    args=parser.parse_args()
    if args.parametros.exists():p=json.loads(args.parametros.read_text())
    else:
        p=PADRAO;args.parametros.write_text(json.dumps(p,ensure_ascii=False,indent=2))
    assert p['motor']['sentido_visto_pela_ponta']=='horario'
    out=args.saida;out.mkdir(parents=True,exist_ok=True)
    inicio=time.time();fio,furos,esq,dir,pontos=gerar_perfil(p)
    print('Perfil de voluta criado.',flush=True)
    cache=BASE/'revisao';cache.mkdir(exist_ok=True)
    h=p['carcaca']['folga_traseira']+p['rotor']['disco']+p['rotor']['altura_pas']+p['carcaca']['folga_frontal']
    formas={}
    for nome,funcao in [('placa',lambda:construir_placa(p,fio,furos)),
                        ('corpo',lambda:construir_voluta(p,fio,furos,esq,dir)[0]),
                        ('tampa',lambda:construir_tampa(p,fio,furos)),
                        ('rotor',lambda:construir_rotor(p))]:
        arquivo=cache/('cache_'+nome+'.brep')
        s=cq.importers.importBrep(str(arquivo)).val() if args.reusar_modelo and arquivo.exists() else funcao()
        s=s.fix().clean()
        cq.exporters.export(s,str(arquivo))
        formas[nome]=s
        print('Criado:',nome,'BRep válido:',s.isValid(),flush=True)
    placa,corpo,tampa,rotor=[formas[n] for n in ['placa','corpo','tampa','rotor']]
    t=p['carcaca']['placa'];zrot=t+p['carcaca']['folga_traseira'];ztampa=t+h
    montados={'01_placa_traseira':placa,'02_corpo_voluta_horario':corpo.translate((0,0,t)),
              '03_tampa_succao_tubo14ext':tampa.translate((0,0,ztampa)),
              '04_rotor_240mm_horario':rotor.translate((0,0,zrot))}
    montados={nome:s.fix().clean() for nome,s in montados.items()}
    imprimir={'01_placa_traseira':placa.rotate((0,0,0),(1,0,0),180).translate((0,0,t)),
              '02_corpo_voluta_horario':corpo,'03_tampa_succao_tubo14ext':tampa,
              '04_rotor_240mm_horario':rotor,
              '05_gabarito_motor':construir_gabarito(p).rotate((0,0,0),(1,0,0),180).translate((0,0,6))}
    for j,folga in enumerate([.2,.3,.4]):
        nome=f'06{chr(65+j)}_teste_eixo_folga_{round(folga*100):03d}'
        m=p['motor'];s=cilindro(10,6).cut(prisma(fio_d(m['diametro_eixo'],m['medida_plano_eixo'],folga),8,-1)).clean()
        imprimir[nome]=s
    for j,diam in enumerate([14.0,14.2,14.4]):
        nome=f'07{chr(65+j)}_teste_tubo_furo_{round(diam*10):03d}'
        imprimir[nome]=cilindro(11,10).cut(cilindro(diam/2,12,-1)).clean()
    auditoria={'estado':p['estado'],'pressao_garantida':False,'rpm_nominal':p['motor']['rpm'],
               'metodo_volume':'Integração de triângulos fechados; não é ensaio estrutural.',
               'deflexao_absoluta_STL_mm':.03,
               'folgas_mm':{'traseira':p['carcaca']['folga_traseira'],'frontal':p['carcaca']['folga_frontal'],
                             'radial_minima':p['carcaca']['folga_radial_minima']},
               'engate_eixo_mm':p['motor']['eixo_ate_ponta']-p['carcaca']['reforco_motor']-zrot,
               'pecas':{},'interferencias_mm3':{}}
    for nome,s in imprimir.items():
        s=s.fix().clean()
        verificar(s,nome);b=s.BoundingBox()
        assert b.zmin>-.001,(nome,b.zmin)
        assert b.xlen<=p['impressao']['mesa_x'] and b.ylen<=p['impressao']['mesa_y'] and b.zlen<=p['impressao']['mesa_z'],(nome,b.xlen,b.ylen,b.zlen)
        s.exportStl(str(out/(nome+'.stl')),tolerance=.03,angularTolerance=.06,relative=False,parallel=True)
        volume=volume_por_triangulos(out/(nome+'.stl'))
        auditoria['pecas'][nome]={'solidos':len(s.Solids()),'brep_valido':s.isValid(),
            'dimensoes_mm':[round(b.xlen,3),round(b.ylen,3),round(b.zlen,3)],'volume_cm3':round(volume/1000,3),
            'massa_solida_PETG_g':round(volume/1000*1.27,2)}
        print('Exportado:',nome,auditoria['pecas'][nome]['dimensoes_mm'],flush=True)
    nomes=list(montados)
    for i,a in enumerate(nomes):
        for b in nomes[i+1:]:
            volume=montados[a].intersect(montados[b]).Volume()
            auditoria['interferencias_mm3'][a+' x '+b]=round(volume,8)
            assert volume<1e-3,('Interferência',a,b,volume)
    envelope=cilindro(p['rotor']['diametro']/2,p['rotor']['disco']+p['rotor']['altura_pas'],zrot)
    for a in nomes[:3]:
        v=envelope.intersect(montados[a]).Volume()
        auditoria['interferencias_mm3']['envelope_girante x '+a]=round(v,8)
        assert v<1e-3,('Interferência no envelope girante',a,v)
    ass=cq.Assembly(name='Exaustor_NB3_prototipo_horario')
    cores=[cq.Color(.27,.40,.48),cq.Color(.12,.58,.69),cq.Color(.68,.77,.82),cq.Color(.93,.48,.16)]
    for (nome,s),cor in zip(montados.items(),cores):ass.add(s,name=nome,color=cor)
    auditoria['altura_camara_mm']=h
    auditoria['montagem_z']={'placa_frente':t,'rotor_base':zrot,'tampa_interna':ztampa,
                            'motor_face':-p['carcaca']['reforco_motor']}
    auditoria['coordenadas_furos_corpo_mm']=furos
    auditoria['tempo_geracao_s']=round(time.time()-inicio,1)
    (out/'verificacao_geometria.json').write_text(json.dumps(auditoria,ensure_ascii=False,indent=2))
    # Formas para visualização e auditoria sem remontar o CAD.
    for nome,s in montados.items():cq.exporters.export(s,str(out/(nome+'_montado.brep')))
    (out/'perfil_voluta.json').write_text(json.dumps({'pontos_espiral_ccw':pontos,'furos':furos,'esquerda_descarga':esq,'direita_descarga':dir},indent=2))
    print(json.dumps(auditoria,ensure_ascii=False,indent=2),flush=True)


if __name__=='__main__':principal()

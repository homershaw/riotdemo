#include "aga_engine.h"
#include "Detail.h"
#include <vector>
#include <string>
#include <math.h>

static float thermalExpand(float diameter_mm,float alpha,float temp_C,float ref_C){
  return diameter_mm*(1.0f+alpha*(temp_C-ref_C));
}

static float rhgCd(float beta,float D_m,float ReD){
  if(beta<=0.0f||beta>=1.0f||D_m<=0.0f||ReD<=0.0f)return 0.0f;
  float D_in=D_m/0.0254f;
  float M1=max(2.8f-D_in,0.0f);
  float L1=1.0f/D_in;
  float L2=1.0f/D_in;
  float A=powf(19000.0f*beta/ReD,0.8f);
  float B=powf(beta,4.0f)/(1.0f-powf(beta,4.0f));
  float M2=2.0f*L2/(1.0f-beta);
  float CiCT=
    0.5961f+
    0.0291f*beta*beta-
    0.2290f*powf(beta,8.0f)+
    0.003f*(1.0f-beta)*M1;
  float upstream=
    (0.0433f+0.0712f*expf(-8.5f*L1)-0.1145f*expf(-6.0f*L1))*
    (1.0f-0.23f*A)*B;
  float downstream=
    -0.0116f*(M2-0.52f*powf(M2,1.3f))*powf(beta,1.1f)*(1.0f-0.14f*A);
  return
    CiCT+upstream+downstream+
    0.000511f*powf(1000000.0f*beta/ReD,0.7f)+
    (0.0210f+0.0049f*A)*powf(beta,4.0f)*powf(1000000.0f/ReD,0.35f);
}

static float y1Upstream(float beta,float dp_kPa,float upstreamAbs_kPa,float kappa){
  if(dp_kPa<=0.0f||upstreamAbs_kPa<=0.0f||kappa<=0.0f)return 1.0f;
  float x1=dp_kPa/upstreamAbs_kPa;
  float y=
    1.0f-
    (0.41f+0.35f*powf(beta,4.0f))*x1/kappa;
  return constrain(y,0.0f,1.0f);
}

void agaEngineBegin(){
  SetupDetail();
}

AGAEngineResult agaEngineCalculate(
  const AGAEngineConfig& cfg,
  const float molPct[21],
  float dp_kPa,
  float staticPressureEU,
  float temp_C
){
  uint32_t started=millis();
  AGAEngineResult r={};
  r.viscosity_cP=0.0103f;

  if(
    cfg.pipeID_mm<=0.0f||
    cfg.orifice_mm<=0.0f||
    cfg.orifice_mm>=cfg.pipeID_mm||
    cfg.basePressure_kPaAbs<=0.0f||
    temp_C<=-273.15f||
    dp_kPa<=0.0f
  ){
    r.calcTime_ms=millis()-started;
    return r;
  }

  std::vector<double> x(22,0.0);
  double sum=0.0;
  for(int i=0;i<21;i++){
    x[i+1]=molPct[i]/100.0;
    sum+=x[i+1];
  }
  if(fabs(sum-1.0)>0.001){
    r.calcTime_ms=millis()-started;
    return r;
  }

  double Mm=0.0;
  MolarMassDetail(x,Mm);

  double Pflow_kPa=cfg.staticPressureIsAbsolute
    ? staticPressureEU
    : staticPressureEU+cfg.atmosphericPressure_kPa;
  double Tflow_K=temp_C+273.15;
  double Dflow=0.0;
  int ierr=0;
  std::string herr;
  DensityDetail(Tflow_K,Pflow_kPa,x,Dflow,ierr,herr);
  r.flowError=ierr;
  if(ierr!=0||Dflow<=0.0){
    r.calcTime_ms=millis()-started;
    return r;
  }

  double P,Zf,dPdD,dPdD2,d2PdTD,dPdT,U,H,S,Cv,Cp,W,G,JT,Kappa;
  PropertiesDetail(Tflow_K,Dflow,x,P,Zf,dPdD,dPdD2,d2PdTD,dPdT,U,H,S,Cv,Cp,W,G,JT,Kappa);
  if(!isfinite(Zf)||Zf<=0.0||!isfinite(Kappa)||Kappa<=0.0){
    r.calcTime_ms=millis()-started;
    return r;
  }

  double Tbase_K=cfg.baseTemperature_C+273.15;
  double Dbase=0.0;
  ierr=0;
  herr.clear();
  DensityDetail(Tbase_K,cfg.basePressure_kPaAbs,x,Dbase,ierr,herr);
  r.baseError=ierr;
  if(ierr!=0||Dbase<=0.0){
    r.calcTime_ms=millis()-started;
    return r;
  }

  double Pb,Zb;
  PropertiesDetail(Tbase_K,Dbase,x,Pb,Zb,dPdD,dPdD2,d2PdTD,dPdT,U,H,S,Cv,Cp,W,G,JT,Kappa);
  if(!isfinite(Zb)||Zb<=0.0){
    r.calcTime_ms=millis()-started;
    return r;
  }

  const float alphaTube=11.7e-6f;
  const float alphaPlate=17.3e-6f;
  const float refTemp_C=20.0f;
  float D_mm=thermalExpand(cfg.pipeID_mm,alphaTube,temp_C,refTemp_C);
  float d_mm=thermalExpand(cfg.orifice_mm,alphaPlate,temp_C,refTemp_C);
  float D_m=D_mm/1000.0f;
  float d_m=d_mm/1000.0f;
  float beta=d_m/D_m;
  if(beta<=0.0f||beta>=1.0f){
    r.calcTime_ms=millis()-started;
    return r;
  }

  float Ev=1.0f/sqrtf(1.0f-powf(beta,4.0f));
  float Y1=y1Upstream(beta,dp_kPa,(float)(Pflow_kPa+dp_kPa),(float)Kappa);

  float MWkg=(float)(Mm/1000.0);
  const float R=8.314462618f;
  float rhoFlow=(float)((Pflow_kPa*1000.0*MWkg)/(Zf*R*Tflow_K));
  float rhoBase=(float)((cfg.basePressure_kPaAbs*1000.0*MWkg)/(Zb*R*Tbase_K));
  if(rhoFlow<=0.0f||rhoBase<=0.0f){
    r.calcTime_ms=millis()-started;
    return r;
  }

  float area=3.14159265358979323846f*d_m*d_m/4.0f;
  float mu_Pa_s=r.viscosity_cP*0.001f;
  float qm=0.25f;
  float Cd=0.60f;
  float ReD=100000.0f;

  for(int iter=0;iter<12;iter++){
    ReD=4.0f*qm/(3.14159265358979323846f*D_m*mu_Pa_s);
    Cd=rhgCd(beta,D_m,ReD);
    float newQm=
      Cd*Y1*Ev*area*sqrtf(2.0f*rhoFlow*dp_kPa*1000.0f);
    if(fabsf(newQm-qm)<0.0000005f){
      qm=newQm;
      break;
    }
    qm=newQm;
  }

  float flow=(qm/rhoBase)*86400.0f/1000.0f;
  r.valid=isfinite(flow);
  r.flow_E3m3_day=flow;
  r.massFlow_kg_s=qm;
  r.Cd=Cd;
  r.Y1=Y1;
  r.Ev=Ev;
  r.beta=beta;
  r.ReD=ReD;
  r.rhoFlow_kg_m3=rhoFlow;
  r.rhoBase_kg_m3=rhoBase;
  r.Zf=(float)Zf;
  r.Zb=(float)Zb;
  r.kappa=(float)Kappa;
  r.calcTime_ms=millis()-started;
  return r;
}

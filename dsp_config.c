/* Strict, versioned persistence for the native DSP control surface. */
#include "squeezelite.h"
#if DSP
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>

#define CONFIG_LIMIT (64 * 1024)
#define OPTION_LIMIT 16384

static char *_read(const char *path) {
	FILE *f=fopen(path,"rb"); long size; char *data;
	if(!f)return NULL;if(fseek(f,0,SEEK_END)||((size=ftell(f))<0)||size>CONFIG_LIMIT||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
	data=malloc((size_t)size+1);if(!data){fclose(f);return NULL;}
	if(fread(data,1,(size_t)size,f)!=(size_t)size){free(data);fclose(f);return NULL;}data[size]='\0';fclose(f);return data;
}
static bool _delimiter(char value){return !value||value==','||value=='}'||value==']';}
static const char *_key(const char *json,const char *key){char needle[80];const char*p=json,*before;size_t length;if(!json||!key||(length=strlen(key))+3>sizeof(needle))return NULL;snprintf(needle,sizeof(needle),"\"%s\"",key);while((p=strstr(p,needle))){before=p;while(before>json&&strchr(" \t\r\n",before[-1]))before--;if(before==json||before[-1]=='{'||before[-1]==','){p+=length+2;while(*p&&strchr(" \t\r\n",*p))p++;if(*p++==':'){while(*p&&strchr(" \t\r\n",*p))p++;return p;}}else p+=length+2;}return NULL;}
static bool _double(const char*json,const char*key,double*v){const char*p=_key(json,key);char*end;if(!p)return false;errno=0;*v=strtod(p,&end);if(errno||end==p||!isfinite(*v))return false;while(*end&&strchr(" \t\r\n",*end))end++;return _delimiter(*end);}
static bool _string(const char*json,const char*key,char*out,size_t cap){const char*p=_key(json,key),*e;size_t n;if(!p||*p!='\"')return false;e=strchr(++p,'\"');if(!e||(n=(size_t)(e-p))>=cap||memchr(p,'\\',n))return false;for(size_t i=0;i<n;i++)if((unsigned char)p[i]<0x20||(unsigned char)p[i]==0x7f)return false;memcpy(out,p,n);out[n]='\0';e++;while(*e&&strchr(" \t\r\n",*e))e++;return _delimiter(*e);}
static bool _append(char*out,size_t cap,const char*fmt,...){va_list ap;size_t used=strlen(out);int n;va_start(ap,fmt);n=vsnprintf(out+used,cap-used,fmt,ap);va_end(ap);return n>=0&&(size_t)n<cap-used;}
static bool _bool(const char*json,const char*key,bool*v){const char*p=_key(json,key),*end;if(!p)return false;if(!strncmp(p,"true",4)){*v=true;end=p+4;}else if(!strncmp(p,"false",5)){*v=false;end=p+5;}else return false;while(*end&&strchr(" \t\r\n",*end))end++;return _delimiter(*end);}
static bool _optional_double(const char*json,const char*key,char*out,size_t cap,const char*option,double minimum,double maximum){double value;if(!_key(json,key))return true;if(!_double(json,key,&value)||value<minimum||value>maximum)return false;return _append(out,cap,"%s=%.8g;",option,value);}
static bool _optional_bool(const char*json,const char*key,char*out,size_t cap,const char*option){bool value;if(!_key(json,key))return true;if(!_bool(json,key,&value))return false;return _append(out,cap,"%s=%s;",option,value?"true":"false");}
static bool _optional_string(const char*json,const char*key,char*out,size_t cap,const char*option,size_t value_cap){char*value;if(!_key(json,key))return true;value=malloc(value_cap);if(!value)return false;if(!_string(json,key,value,value_cap)||strchr(value,';')){free(value);return false;}if(!*value){free(value);return true;}if(!_append(out,cap,"%s=%s;",option,value)){free(value);return false;}free(value);return true;}
static bool _optional_bool_array12(const char*json,const char*key,char*out,size_t cap,const char*option){const char*p=_key(json,key);unsigned i;if(!p)return true;if(*p++!='['||!_append(out,cap,"%s=",option))return false;for(i=0;i<12;i++){bool value;while(*p&&strchr(" \t\r\n",*p))p++;if(!strncmp(p,"true",4)){value=true;p+=4;}else if(!strncmp(p,"false",5)){value=false;p+=5;}else return false;if(!_append(out,cap,"%s%s",i?",":"",value?"true":"false"))return false;while(*p&&strchr(" \t\r\n",*p))p++;if(i<11&&*p++!=',')return false;}return *p==']'&&_append(out,cap,";");}

char *dsp_config_load_options(const char *path,const char *expected_player) {
	char *json=_read(path),player[128],headroom[32],filters[8192]="",*out;const char*p;double version,preamp,gain;unsigned i;
	if(!json)return NULL;
	p=json;while(*p&&strchr(" \t\r\n",*p))p++;if(*p!='{'){free(json);return NULL;}{const char*end=json+strlen(json);while(end>json&&strchr(" \t\r\n",end[-1]))end--;if(end==json||end[-1]!='}'){free(json);return NULL;}}
	if(!_double(json,"version",&version)||(version!=1&&version!=2)||!_string(json,"player_id",player,sizeof(player))||(expected_player&&*expected_player&&strcmp(player,expected_player))||!_double(json,"preamp_db",&preamp)||preamp< -60||preamp>24){free(json);return NULL;}
	out=calloc(1,OPTION_LIMIT);if(!out){free(json);return NULL;}snprintf(out,OPTION_LIMIT,"preamp=%.8g;",preamp);
	p=_key(json,"headroom_db");if(!p){free(json);free(out);return NULL;}if(!strncmp(p,"null",4)&&_delimiter(p[4]))strcat(out,"headroom=auto;");else{char*end;errno=0;gain=strtod(p,&end);while(*end&&strchr(" \t\r\n",*end))end++;if(errno||end==p||!isfinite(gain)||gain<0||gain>60||!_delimiter(*end)){free(json);free(out);return NULL;}snprintf(headroom,sizeof(headroom),"headroom=%.8g;",gain);strcat(out,headroom);}
	{bool bypass;if(!_bool(json,"bypass",&bypass)||!_append(out,OPTION_LIMIT,"bypass=%s;",bypass?"true":"false")){free(json);free(out);return NULL;}}
	p=_key(json,"graphic_eq_db");if(!p||*p++!='['||!_append(out,OPTION_LIMIT,"eq=")){free(json);free(out);return NULL;}for(i=0;i<12;i++){char*end;while(*p&&strchr(" \t\r\n",*p))p++;errno=0;gain=strtod(p,&end);if(errno||end==p||!isfinite(gain)||gain< -24||gain>24){free(json);free(out);return NULL;}if(!_append(out,OPTION_LIMIT,"%s%.8g",i?",":"",gain)){free(json);free(out);return NULL;}p=end;while(*p&&strchr(" \t\r\n",*p))p++;if(i<11&&*p++!=','){free(json);free(out);return NULL;}}if(*p!=']'||!_append(out,OPTION_LIMIT,";")){free(json);free(out);return NULL;}
	if(!_string(json,"parametric",filters,sizeof(filters))||(*filters&&!_append(out,OPTION_LIMIT,"%s%s",filters,filters[strlen(filters)-1]==';'?"":";"))){free(json);free(out);return NULL;}
	if(version==2){
		if(!_optional_bool(json,"eq_bypass",out,OPTION_LIMIT,"eq_bypass")||
			!_optional_bool_array12(json,"graphic_eq_enabled",out,OPTION_LIMIT,"eq_enabled")||
			!_optional_bool(json,"parametric_bypass",out,OPTION_LIMIT,"parametric_bypass")||
			!_optional_bool(json,"fir_bypass",out,OPTION_LIMIT,"fir_bypass")||
			!_optional_bool(json,"spatial_bypass",out,OPTION_LIMIT,"spatial_bypass")||
			!_optional_string(json,"fir_file",out,OPTION_LIMIT,"fir",PATH_MAX)||
			!_optional_double(json,"fir_gain_db",out,OPTION_LIMIT,"fir_gain",-60,24)||
			!_optional_bool(json,"fir_normalize",out,OPTION_LIMIT,"fir_normalize")||
			!_optional_double(json,"fir_max_taps",out,OPTION_LIMIT,"fir_max_taps",512,262144)||
			!_optional_double(json,"fir_trim_db",out,OPTION_LIMIT,"fir_trim",-180,-20)||
			!_optional_string(json,"fir_channel_map",out,OPTION_LIMIT,"fir_channel_map",16)||
			!_optional_string(json,"fir_latency_reference",out,OPTION_LIMIT,"fir_latency",16)||
			!_optional_double(json,"balance",out,OPTION_LIMIT,"balance",-1,1)||
			!_optional_double(json,"stereo_width",out,OPTION_LIMIT,"width",0,2)||
			!_optional_bool(json,"mono",out,OPTION_LIMIT,"mono")||
			!_optional_string(json,"polarity",out,OPTION_LIMIT,"polarity",16)||
			!_optional_double(json,"delay_left_ms",out,OPTION_LIMIT,"delay_l_ms",0,100)||
			!_optional_double(json,"delay_right_ms",out,OPTION_LIMIT,"delay_r_ms",0,100)||
			!_optional_string(json,"crossfeed",out,OPTION_LIMIT,"crossfeed",16)||
			!_optional_double(json,"loudness_db",out,OPTION_LIMIT,"loudness",0,12)||
			!_optional_bool(json,"true_peak",out,OPTION_LIMIT,"true_peak")||
			!_optional_bool(json,"limiter",out,OPTION_LIMIT,"limiter")||
			!_optional_double(json,"limiter_ceiling_db",out,OPTION_LIMIT,"limiter_ceiling",-12,0)||
			!_optional_double(json,"replaygain_db",out,OPTION_LIMIT,"replaygain",-30,30)||
			!_optional_bool(json,"replaygain_managed",out,OPTION_LIMIT,"replaygain_managed")||
			!_optional_bool(json,"replaygain_headroom",out,OPTION_LIMIT,"replaygain_headroom")){
			free(json);free(out);return NULL;
		}
	}
	if(!dsp_validate(out)){free(json);free(out);return NULL;}
	free(json);return out;
}

bool dsp_config_save(const char *path,const char *player,const char *json) {
	char tmp[PATH_MAX],bak[PATH_MAX];FILE*f;char*validated;int fd;
	if(!path||!player||!json||snprintf(tmp,sizeof(tmp),"%s.tmp",path)>=(int)sizeof(tmp)||snprintf(bak,sizeof(bak),"%s.bak",path)>=(int)sizeof(bak))return false;
	/* Validate the exact document before it can replace a working configuration. */
	f=fopen(tmp,"wb");if(!f)return false;if(fwrite(json,1,strlen(json),f)!=strlen(json)||fflush(f)||(fd=fileno(f),fsync(fd))){fclose(f);unlink(tmp);return false;}fclose(f);
	validated=dsp_config_load_options(tmp,player);if(!validated){unlink(tmp);return false;}free(validated);
	(void)unlink(bak);if(access(path,F_OK)==0&&rename(path,bak)){unlink(tmp);return false;}if(rename(tmp,path)){if(access(bak,F_OK)==0)(void)rename(bak,path);unlink(tmp);return false;}return true;
}

bool dsp_config_rollback(const char *path){char bak[PATH_MAX],bad[PATH_MAX];if(!path||snprintf(bak,sizeof(bak),"%s.bak",path)>=(int)sizeof(bak)||access(bak,R_OK))return false;snprintf(bad,sizeof(bad),"%s.rejected",path);(void)unlink(bad);if(access(path,F_OK)==0&&(rename(path,bad)))return false;if(rename(bak,path)){if(access(bad,F_OK)==0)(void)rename(bad,path);return false;}return true;}
#endif

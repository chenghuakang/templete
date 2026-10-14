#include "core.hpp"
#include "oneweb_small.hpp"
#include "rk8.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <climits>
#include <cctype>
#include <cerrno>
#include <locale>
#include <cmath>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#ifdef _OPENMP
#include <omp.h>
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif
namespace sp {
namespace fs=std::filesystem;
std::string pathtext(const fs::path&p){auto u=p.u8string();return std::string(reinterpret_cast<const char*>(u.data()),u.size());}
using U=uint64_t;
using I=int64_t;
using B=std::vector<uint8_t>;
constexpr double pi=3.14159265358979323846, RE=6378137, RB=6356752.314245, mu=3.986004418e14, AU=149597870700.0;
struct Err:std::runtime_error{
int code;
std::string where;
Err(int c,std::string w,std::string s):runtime_error(s),code(c),where(std::move(w)){
}
}
;
[[noreturn]] void fail(int c,const std::string&w,const std::string&m){
throw Err(c,w,m);
}
void need(bool ok,int c,const std::string&w,const std::string&m){
if(!ok)fail(c,w,m);
}
U add(U a,U b){
need(b<=UINT64_MAX-a,7,"size","addition overflow");
return a+b;
}
U mul(U a,U b){
need(!a||b<=UINT64_MAX/a,7,"size","multiplication overflow");
return a*b;
}
U align(U a,U n){
return mul((add(a,n-1)/n),n);
}
volatile std::sig_atomic_t cancelled=0;
void signal_handler(int){
cancelled=1;
}
void safe(){
if(cancelled)fail(3,"run","cancelled");
}
std::string readtext(const fs::path&p,U limit=16*1024*1024){
std::ifstream f(p,std::ios::binary);
need(bool(f),3,"open",pathtext(p));
U n=fs::file_size(p);
need(n<=limit,2,"input","file exceeds limit");
std::string s(static_cast<size_t>(n),'\0');
f.read(s.data(),static_cast<std::streamsize>(n));
need(bool(f)||n==0,3,"read",pathtext(p));
return s;
}
std::string trim(std::string s){
auto a=s.find_first_not_of(" \r\n\t");
if(a==std::string::npos)return {
}
;
return s.substr(a,s.find_last_not_of(" \r\n\t")-a+1);
}
void utfappend(std::string&s,uint32_t c){
need(c<=0x10ffff&&!(c>=0xd800&&c<=0xdfff)&&c!=0,2,"unicode","invalid scalar");
if(c<128)s+=char(c);
else if(c<2048){
s+=char(0xc0|(c>>6));
s+=char(0x80|(c&63));
}
else if(c<65536){
s+=char(0xe0|(c>>12));
s+=char(0x80|((c>>6)&63));
s+=char(0x80|(c&63));
}
else{
s+=char(0xf0|(c>>18));
s+=char(0x80|((c>>12)&63));
s+=char(0x80|((c>>6)&63));
s+=char(0x80|(c&63));
}
}
void utfcheck(const std::string&s){
for(size_t i=0;i<s.size();){
unsigned c=uint8_t(s[i++]);
if(c<128){
need(c!=0,2,"utf8","NUL");
continue;
}
int n=c>=0xf0?3:c>=0xe0?2:c>=0xc2?1:0;
need(n&&c<=0xf4&&i+n<=s.size(),2,"utf8","invalid sequence");
unsigned v=c&((1u<<(6-n))-1);
for(int j=0;j<n;++j){
unsigned d=uint8_t(s[i++]);
need((d&0xc0)==0x80,2,"utf8","invalid continuation");
v=(v<<6)|(d&63);
}
need(v>=(n==1?128u:n==2?2048u:65536u)&&v<=0x10ffff&&!(v>=0xd800&&v<=0xdfff),2,"utf8","invalid scalar");
}
}
std::string cut(std::string s,size_t n){
if(s.size()<=n)return s;
size_t k=n;
while(k&&((uint8_t(s[k])&0xc0)==0x80))--k;
s.resize(k);
return s;
}
std::string quote(const std::string&s){
std::string t="\"";
const char*h="0123456789abcdef";
for(unsigned char c:s){
if(c=='"'||c=='\\'){
t+='\\';
t+=char(c);
}
else if(c<32){
t+="\\u00";
t+=h[c>>4];
t+=h[c&15];
}
else t+=char(c);
}
return t+'"';
}
std::string number(double x){
need(std::isfinite(x),5,"format","nonfinite");
char b[64];
auto r=std::to_chars(b,b+64,x);
need(r.ec==std::errc{
}
,5,"format","to_chars");
return std::string(b,r.ptr);
}
struct J{
enum Type{
Null,Bool,Num,Str,Arr,Obj}
type=Null;
std::string s;
bool b=false;
std::vector<J>a;
std::map<std::string,J>o;
J()=default;
J(const char*v):type(Str),s(v){
}
J(std::string v):type(Str),s(std::move(v)){
}
J(bool v):type(Bool),b(v){
}
J(double v):type(Num),s(number(v)){
}
J(U v):type(Num),s(std::to_string(v)){
}
J(I v):type(Num),s(std::to_string(v)){
}
J(int v):J(I(v)){
}
static J obj(){
J j;
j.type=Obj;
return j;
}
static J arr(){
J j;
j.type=Arr;
return j;
}
J&operator[](const std::string&k){
need(type==Obj,2,"json","expected object");
return o[k];
}
const J&at(const std::string&k)const{
auto it=o.find(k);
need(type==Obj&&it!=o.end(),2,"json","missing key "+k);
return it->second;
}
std::string str()const{
need(type==Str,2,"json","expected string");
return s;
}
double real()const{
need(type==Num,2,"json","expected number");
double x;
auto r=std::from_chars(s.data(),s.data()+s.size(),x);
need(r.ec==std::errc{
}
&&r.ptr==s.data()+s.size()&&std::isfinite(x),2,"json","invalid number");
return x;
}
U u()const{
need(type==Num,2,"json","expected integer");
U x;
auto r=std::from_chars(s.data(),s.data()+s.size(),x);
need(r.ec==std::errc{
}
&&r.ptr==s.data()+s.size(),2,"json","invalid uint64");
return x;
}
I i()const{
need(type==Num,2,"json","expected integer");
I x;
auto r=std::from_chars(s.data(),s.data()+s.size(),x);
need(r.ec==std::errc{
}
&&r.ptr==s.data()+s.size(),2,"json","invalid int64");
return x;
}
bool flag()const{
need(type==Bool,2,"json","expected bool");
return b;
}
std::string dump()const{
if(type==Null)return "null";
if(type==Bool)return b?"true":"false";
if(type==Num)return s;
if(type==Str)return quote(s);
std::string r=type==Arr?"[":"{";
bool first=true;
if(type==Arr){
for(auto&v:a){
if(!first)r+=',';
first=false;
r+=v.dump();
}
}
else for(auto&kv:o){
if(!first)r+=',';
first=false;
r+=quote(kv.first)+":"+kv.second.dump();
}
return r+(type==Arr?"]":"}");
}
}
;
struct JP{
std::string s;
size_t p=0;
explicit JP(std::string t):s(std::move(t)){
if(s.compare(0,3,"\xef\xbb\xbf")==0)s.erase(0,3);
utfcheck(s);
}
void ws(){
while(p<s.size()&&std::string(" \n\r\t").find(s[p])!=std::string::npos)++p;
}
char get(){
need(p<s.size(),2,"json","unexpected end");
return s[p++];
}
uint32_t hex(){
uint32_t v=0;
for(int i=0;i<4;++i){
char c=get();
int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
need(n>=0,2,"json","bad hex");
v=v*16+unsigned(n);
}
return v;
}
std::string string(){
need(get()=='"',2,"json","expected string");
std::string r;
for(;;){
char c=get();
if(c=='"')break;
need(uint8_t(c)>=32,2,"json","control character");
if(c!='\\'){
r+=c;
continue;
}
c=get();
if(c=='"'||c=='\\'||c=='/')r+=c;
else if(c=='b')r+='\b';
else if(c=='f')r+='\f';
else if(c=='n')r+='\n';
else if(c=='r')r+='\r';
else if(c=='t')r+='\t';
else if(c=='u'){
auto v=hex();
if(v>=0xd800&&v<=0xdbff){
need(get()=='\\'&&get()=='u',2,"json","surrogate pair");
auto w=hex();
need(w>=0xdc00&&w<=0xdfff,2,"json","surrogate pair");
v=0x10000+(v-0xd800)*1024+w-0xdc00;
}
if(v==0)r+='\0';
else utfappend(r,v);
}
else fail(2,"json","escape");
}
return r;
}
J value(int depth=0){
need(depth<=64,2,"json","depth");
ws();
need(p<s.size(),2,"json","end");
char c=s[p];
if(c=='"')return J(string());
if(c=='{'||c=='['){
++p;
J r=c=='{'?J::obj():J::arr();
ws();
if(p<s.size()&&s[p]==(c=='{'?'}':']')){
++p;
return r;
}
for(;;){
ws();
if(c=='{'){
auto k=string();
ws();
need(get()==':',2,"json","colon");
need(!r.o.count(k),2,"json","duplicate key");
r.o.emplace(k,value(depth+1));
}
else r.a.push_back(value(depth+1));
ws();
char d=get();
if(d==(c=='{'?'}':']'))break;
need(d==',',2,"json","separator");
}
return r;
}
for(auto t:{
"true","false","null"}
)if(s.compare(p,std::strlen(t),t)==0){
p+=std::strlen(t);
if(std::string(t)=="null")return J();
return J(std::string(t)=="true");
}
size_t a=p;
if(s[p]=='-')++p;
need(p<s.size()&&std::isdigit(uint8_t(s[p])),2,"json","number");
if(s[p]=='0')++p;
else while(p<s.size()&&std::isdigit(uint8_t(s[p])))++p;
if(p<s.size()&&s[p]=='.'){
++p;
size_t q=p;
while(p<s.size()&&std::isdigit(uint8_t(s[p])))++p;
need(p>q,2,"json","fraction");
}
if(p<s.size()&&(s[p]=='e'||s[p]=='E')){
++p;
if(p<s.size()&&(s[p]=='+'||s[p]=='-'))++p;
size_t q=p;
while(p<s.size()&&std::isdigit(uint8_t(s[p])))++p;
need(p>q,2,"json","exponent");
}
J r;
r.type=J::Num;
r.s=s.substr(a,p-a);
r.real();
return r;
}
J parse(){
auto r=value();
ws();
need(p==s.size(),2,"json","trailing data");
return r;
}
}
;
J json(const std::string&s){
return JP(s).parse();
}
struct X{
std::string tag;
std::map<std::string,std::string>a;
std::vector<X>c;
}
;
struct XP{
std::string s;
size_t p=0;
XP(std::string t):s(std::move(t)){
if(s.compare(0,3,"\xef\xbb\xbf")==0)s.erase(0,3);
utfcheck(s);
}
void ws(){
while(p<s.size()&&std::isspace(uint8_t(s[p])))++p;
}
bool starts(const char*t){
return s.compare(p,std::strlen(t),t)==0;
}
std::string name(){
size_t a=p;
while(p<s.size()&&(std::isalnum(uint8_t(s[p]))||s[p]=='_'||s[p]=='-'))++p;
need(p>a,2,"xml","expected name at byte "+std::to_string(p));
return s.substr(a,p-a);
}
void skip(){
for(;;){
ws();
if(starts("<!--")){
auto e=s.find("-->",p+4);
need(e!=std::string::npos,2,"xml","comment");
p=e+3;
}
else break;
}
}
std::string entity(std::string t){
std::string r;
for(size_t i=0;i<t.size();++i){
if(t[i]!='&'){
r+=t[i];
continue;
}
auto e=t.find(';',i);
need(e!=std::string::npos,2,"xml","entity");
auto v=t.substr(i+1,e-i-1);
if(v=="lt")r+='<';
else if(v=="gt")r+='>';
else if(v=="amp")r+='&';
else if(v=="quot")r+='"';
else if(v=="apos")r+='\'';
else if(v.size()>1&&v[0]=='#'){
unsigned c=0;
bool h=v[1]=='x';
auto q=v.substr(h?2:1);
auto z=std::from_chars(q.data(),q.data()+q.size(),c,h?16:10);
need(z.ec==std::errc{
}
&&z.ptr==q.data()+q.size(),2,"xml","numeric entity");
utfappend(r,c);
}
else fail(2,"xml","unknown entity");
i=e;
}
return r;
}
X element(int d=0){
need(d<64,2,"xml","depth");
skip();
need(p<s.size()&&s[p++]=='<',2,"xml","expected element");
X x;
x.tag=name();
for(;;){
ws();
if(starts("/>")){
p+=2;
return x;
}
if(starts(">")){
++p;
break;
}
auto k=name();
ws();
need(p<s.size()&&s[p++]=='=',2,"xml","equals");
ws();
need(p<s.size()&&(s[p]=='\''||s[p]=='"'),2,"xml","quote");
char q=s[p++];
auto e=s.find(q,p);
need(e!=std::string::npos,2,"xml","attribute");
auto v=entity(s.substr(p,e-p));
need(!x.a.count(k),2,"xml","duplicate attribute");
x.a[k]=trim(v);
p=e+1;
}
for(;;){
skip();
if(starts("</")){
p+=2;
need(name()==x.tag,2,"xml","closing tag");
ws();
need(p<s.size()&&s[p++]=='>',2,"xml","closing >");
return x;
}
if(starts("<![CDATA[")){
auto e=s.find("]]>",p+9);
need(e!=std::string::npos&&trim(s.substr(p+9,e-p-9)).empty(),2,"xml","nonwhite CDATA");
p=e+3;
continue;
}
need(p<s.size()&&s[p]=='<',2,"xml","nonwhite text");
x.c.push_back(element(d+1));
}
}
X parse(){
skip();
if(starts("<?xml")){
auto e=s.find("?>",p);
need(e!=std::string::npos,2,"xml","declaration");
p=e+2;
}
auto x=element();
skip();
need(p==s.size(),2,"xml","trailing content");
return x;
}
}
;
void keys(const X&x,const std::string&list){
std::set<std::string> k;
std::istringstream f(list);
std::string v;
while(f>>v)k.insert(v);
for(auto&a:x.a)need(k.count(a.first),2,"xml","unknown attribute "+a.first+" in "+x.tag);
}
std::string attr(const X&x,const std::string&k,std::string def=""){
auto i=x.a.find(k);
return i==x.a.end()?def:i->second;
}
double real(const std::string&s){
double v;
auto r=std::from_chars(s.data(),s.data()+s.size(),v);
need(r.ec==std::errc{
}
&&r.ptr==s.data()+s.size()&&std::isfinite(v),2,"number",s);
return v;
}
U uint(const std::string&s){
U v;
auto r=std::from_chars(s.data(),s.data()+s.size(),v);
need(r.ec==std::errc{
}
&&r.ptr==s.data()+s.size(),2,"integer",s);
return v;
}
double xr(const X&x,const char*k,double d){
return x.a.count(k)?real(x.a.at(k)):d;
}
U xu(const X&x,const char*k,U d){
return x.a.count(k)?uint(x.a.at(k)):d;
}
uint32_t crc(const uint8_t*p,size_t n){
static std::array<uint32_t,256>tab=[](){
std::array<uint32_t,256>t{
}
;
for(unsigned i=0;i<256;++i){
uint32_t c=i;
for(int j=0;j<8;++j)c=(c>>1)^((c&1)?0x82f63b78u:0u);
t[i]=c;
}
return t;
}
();
uint32_t c=~0u;
for(size_t i=0;i<n;++i)c=tab[(c^p[i])&255]^(c>>8);
return ~c;
}
uint32_t crc(const B&b){
return crc(b.data(),b.size());
}
struct SHA{
uint32_t h[8]={
0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}
;
std::array<uint8_t,64>buf{
}
;
U count=0;
size_t used=0;
static uint32_t rr(uint32_t a,unsigned b){
return(a>>b)|(a<<(32-b));
}
void block(const uint8_t*p){
static constexpr uint32_t k[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2}
;
uint32_t w[64];
for(int i=0;i<16;++i)w[i]=(uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|(uint32_t(p[i*4+2])<<8)|p[i*4+3];
for(int i=16;i<64;++i){
uint32_t a=w[i-15],b=w[i-2];
w[i]=w[i-16]+(rr(a,7)^rr(a,18)^(a>>3))+w[i-7]+(rr(b,17)^rr(b,19)^(b>>10));
}
uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
for(int i=0;i<64;++i){
uint32_t t=z+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^(~e&g))+k[i]+w[i],v=(rr(a,2)^rr(a,13)^rr(a,22))+((a&b)^(a&c)^(b&c));
z=g;
g=f;
f=e;
e=d+t;
d=c;
c=b;
b=a;
a=t+v;
}
h[0]+=a;
h[1]+=b;
h[2]+=c;
h[3]+=d;
h[4]+=e;
h[5]+=f;
h[6]+=g;
h[7]+=z;
}
void update(const uint8_t*p,size_t n){
count+=n;
while(n){
size_t a=std::min(n,64-used);
std::memcpy(buf.data()+used,p,a);
used+=a;
p+=a;
n-=a;
if(used==64){
block(buf.data());
used=0;
}
}
}
B finish(){
U bits=count*8;
uint8_t v=128;
update(&v,1);
v=0;
while(used!=56)update(&v,1);
uint8_t z[8];
for(int i=0;i<8;++i)z[7-i]=uint8_t(bits>>(8*i));
update(z,8);
B r(32);
for(int i=0;i<8;++i)for(int j=0;j<4;++j)r[i*4+j]=uint8_t(h[i]>>(24-8*j));
return r;
}
}
;
B sha(const std::string&s){
SHA h;
h.update(reinterpret_cast<const uint8_t*>(s.data()),s.size());
return h.finish();
}
B sha(const B&s){
SHA h;
h.update(s.data(),s.size());
return h.finish();
}
B filesha(const fs::path&p){
std::ifstream f(p,std::ios::binary);
need(bool(f),3,"sha",pathtext(p));
SHA h;
std::array<uint8_t,65536>b;
while(f){
f.read(reinterpret_cast<char*>(b.data()),b.size());
h.update(b.data(),static_cast<size_t>(f.gcount()));
}
need(f.eof(),3,"sha","read failed");
return h.finish();
}
std::string hex(const B&b){
std::string s;
const char*h="0123456789abcdef";
for(auto c:b){
s+=h[c>>4];
s+=h[c&15];
}
return s;
}
void put(B&b,U o,U v,int n){
need(add(o,U(n))<=b.size(),5,"serialize","bounds");
for(int i=0;i<n;++i)b[size_t(o)+i]=uint8_t(v>>(8*i));
}
U get(const B&b,U o,int n){
need(add(o,U(n))<=b.size(),7,"decode","bounds");
U v=0;
for(int i=0;i<n;++i)v|=U(b[size_t(o)+i])<<(8*i);
return v;
}
void pf(B&b,U o,double x){
U v;
std::memcpy(&v,&x,8);
put(b,o,v,8);
}
double gf(const B&b,U o){
U v=get(b,o,8);
double x;
std::memcpy(&x,&v,8);
return x;
}
void ps(B&b,U o,const std::string&s,size_t n){
need(add(o,n)<=b.size(),5,"serialize","string bounds");
std::memcpy(b.data()+o,s.data(),std::min(s.size(),n));
}
std::string gs(const B&b,U o,size_t n){
need(add(o,n)<=b.size(),7,"decode","string bounds");
std::string s(reinterpret_cast<const char*>(b.data()+o),n);
auto p=s.find('\0');
if(p!=std::string::npos)s.resize(p);
return s;
}
void append(B&b,U v,int n){
size_t o=b.size();
b.resize(o+n);
put(b,o,v,n);
}
void appendf(B&b,double v){
size_t o=b.size();
b.resize(o+8);
pf(b,o,v);
}
void zero(const B&b,U a,U e){
need(a<=e&&e<=b.size(),7,"gap","bounds");
for(U i=a;i<e;++i)need(b[size_t(i)]==0,7,"gap","nonzero reserved/padding");
}
B load(const fs::path&p,U budget){
U n=fs::file_size(p);
need(n<=budget&&n<=SIZE_MAX,2,"memory","reader package exceeds memory budget");
auto s=readtext(p,budget);
return B(s.begin(),s.end());
}
void atomic(const fs::path&p,const B&b){
safe();
fs::create_directories(p.parent_path());
auto t=p;
t+=".part";
try{
std::ofstream f(t,std::ios::binary|std::ios::trunc);
need(bool(f),3,"write",pathtext(t));
f.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));
f.flush();
need(bool(f),3,"write","flush failed");
f.close();
safe();
#ifdef _WIN32
need(MoveFileExW(t.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,3,"rename",pathtext(p));
#else
fs::rename(t,p);
#endif
}
catch(...){
std::error_code ec;
fs::remove(t,ec);
throw;
}
}
void atomic(const fs::path&p,const std::string&s){
atomic(p,B(s.begin(),s.end()));
}
I days(int y,unsigned m,unsigned d){
y-=m<=2;
int e=(y>=0?y:y-399)/400;
unsigned z=unsigned(y-e*400),q=(153*(m+(m>2?unsigned(-3):9))+2)/5+d-1;
return I(e)*146097+z*365+z/4-z/100+q-719468;
}
I utc(const std::string&s){
need(s.size()>=20&&s[4]=='-'&&s[7]=='-'&&s[10]=='T'&&s[13]==':'&&s[16]==':'&&s.back()=='Z',2,"utc","ISO UTC required");
auto n=[&](size_t p,size_t l){
return int(uint(s.substr(p,l)));
}
;
int y=n(0,4),m=n(5,2),d=n(8,2),h=n(11,2),mi=n(14,2),sec=n(17,2);
int md[]={
31,28,31,30,31,30,31,31,30,31,30,31}
;
need(y>=1970&&y<=2261&&m>=1&&m<=12,2,"utc","calendar range");
if(y%4==0&&(y%100!=0||y%400==0))md[1]=29;
need(d>=1&&d<=md[m-1]&&h<24&&mi<60&&sec<60,2,"utc","calendar invalid");
I f=0;
if(s.size()!=20){
need(s[19]=='.'&&s.size()>=22&&s.size()<=30,2,"utc","fraction length");
std::string z=s.substr(20,s.size()-21);
f=I(uint(z));
for(size_t k=z.size();k<9;++k)f*=10;
}
U t=mul(U(days(y,unsigned(m),unsigned(d))*86400+h*3600+mi*60+sec),1000000000);
need(add(t,U(f))<=U(INT64_MAX),2,"utc","overflow");
return I(t+U(f));
}
std::string iso(I t){
I z=t/1000000000/86400+719468;
I era=z/146097;
unsigned doe=unsigned(z-era*146097),yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
int y=int(yoe+era*400);
unsigned doy=doe-(365*yoe+yoe/4-yoe/100),mp=(5*doy+2)/153,d=doy-(153*mp+2)/5+1,m=mp+(mp<10?3:unsigned(-9));
y+=m<=2;
I q=t/1000000000%86400;
std::ostringstream o;
o<<std::setfill('0')<<std::setw(4)<<y<<'-'<<std::setw(2)<<m<<'-'<<std::setw(2)<<d<<'T'<<std::setw(2)<<q/3600<<':'<<std::setw(2)<<q/60%60<<':'<<std::setw(2)<<q%60<<'.'<<std::setw(9)<<t%1000000000<<'Z';
return o.str();
}
I seconds(std::string s){
need(!s.empty(),1,"seconds","empty");
bool neg=false;
if(s[0]=='+'||s[0]=='-'){
neg=s[0]=='-';
s.erase(0,1);
}
int exp=0;
auto e=s.find_first_of("eE");
if(e!=std::string::npos){
auto t=s.substr(e+1);
if(!t.empty()&&t[0]=='+')t.erase(0,1);
auto r=std::from_chars(t.data(),t.data()+t.size(),exp);
need(r.ec==std::errc{
}
&&r.ptr==t.data()+t.size()&&std::abs(exp)<=1000,1,"seconds","exponent");
s.resize(e);
}
auto dot=s.find('.');
int frac=0;
if(dot!=std::string::npos){
need(s.find('.',dot+1)==std::string::npos&&dot>0&&dot+1<s.size(),1,"seconds","decimal");
frac=int(s.size()-dot-1);
s.erase(dot,1);
}
need(!s.empty()&&s.find_first_not_of("0123456789")==std::string::npos,1,"seconds","invalid decimal");
auto nz=s.find_first_not_of('0');
if(nz==std::string::npos)return 0;
s.erase(0,nz);
int shift=9+exp-frac;
U v=0;
if(shift>=0){
need(s.size()+size_t(shift)<=19,1,"seconds","range");
s.append(size_t(shift),'0');
v=uint(s);
}
else {
size_t cutn=size_t(-shift);
std::string whole=cutn>=s.size()?"0":s.substr(0,s.size()-cutn);
v=uint(whole);
std::string rem=cutn>=s.size()?std::string(cutn-s.size(),'0')+s:s.substr(s.size()-cutn);
bool up=rem[0]>'5'||(rem[0]=='5'&&(rem.find_first_not_of('0',1)!=std::string::npos||(v&1)));
if(up)v=add(v,1);
}
need(v<=U(INT64_MAX),1,"seconds","range");
return neg?-I(v):I(v);
}
struct V{
double x=0,y=0,z=0;
V operator+(V b)const{
return{
x+b.x,y+b.y,z+b.z}
;
}
V operator-(V b)const{
return{
x-b.x,y-b.y,z-b.z}
;
}
V operator*(double a)const{
return{
x*a,y*a,z*a}
;
}
}
;
double dot(V a,V b){
return a.x*b.x+a.y*b.y+a.z*b.z;
}
double norm(V a){
return std::sqrt(dot(a,a));
}
struct Mat{
double a[9]{
}
;
V operator*(V v)const{
return{
a[0]*v.x+a[1]*v.y+a[2]*v.z,a[3]*v.x+a[4]*v.y+a[5]*v.z,a[6]*v.x+a[7]*v.y+a[8]*v.z}
;
}
Mat operator*(const Mat&b)const{
Mat c;
for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)c.a[i*3+j]+=a[i*3+k]*b.a[k*3+j];
return c;
}
Mat tr()const{
Mat b;
for(int i=0;i<3;++i)for(int j=0;j<3;++j)b.a[i*3+j]=a[j*3+i];
return b;
}
}
;
Mat rx(double t){
double c=std::cos(t),s=std::sin(t);
return{
{
1,0,0,0,c,-s,0,s,c}
}
;
}
Mat ry(double t){
double c=std::cos(t),s=std::sin(t);
return{
{
c,0,s,0,1,0,-s,0,c}
}
;
}
Mat rz(double t){
double c=std::cos(t),s=std::sin(t);
return{
{
c,-s,0,s,c,0,0,0,1}
}
;
}
double wrap(double a){
a=std::fmod(a,2*pi);
return a<0?a+2*pi:a==0?0:a;
}
struct State{
std::array<double,6>q{
}
;
V r()const{
return{
q[0],q[1],q[2]}
;
}
V v()const{
return{
q[3],q[4],q[5]}
;
}
}
;
State state(V r,V v){
return{
{
r.x,r.y,r.z,v.x,v.y,v.z}
}
;
}
State kepler(double a,double e,double inc,double om,double u,double w){
double M=wrap(u-w),lo=0,hi=2*pi;
for(int j=0;j<60;++j){
double E=(lo+hi)/2;
if(E-e*std::sin(E)<M)lo=E;
else hi=E;
}
double E=(lo+hi)/2,n=std::sqrt(mu/(a*a*a)),v=std::sqrt(1-e*e),d=1-e*std::cos(E);
Mat Q=rz(om)*rx(inc)*rz(w);
return state(Q*V{
a*(std::cos(E)-e),a*v*std::sin(E),0}
,Q*V{
-a*n*std::sin(E)/d,a*n*v*std::cos(E)/d,0}
);
}
struct Radio{
U up=10000000000ULL,down=10000000000ULL;
uint32_t ant=6;
}
;
struct Phys{
double mass=260,drag=10,cd=2.2,srp=10,cr=1.3;
std::string source="placeholder",reference;
}
;
struct PhysOv{
bool mass=false,drag=false,cd=false,srp=false,cr=false,source=false,reference=false;
double mass_v=0,drag_v=0,cd_v=0,srp_v=0,cr_v=0;
std::string source_v,reference_v;
}
;
void radio(const X&x,Radio&r){
keys(x,"uplink_bps downlink_bps antennas");
r.up=xu(x,"uplink_bps",r.up);
r.down=xu(x,"downlink_bps",r.down);
U a=xu(x,"antennas",r.ant);
need(a<=UINT32_MAX,2,"radio","antennas overflow");
r.ant=uint32_t(a);
}
void phys(const X&x,Phys&p){
keys(x,"mass_kg drag_area_m2 cd srp_area_m2 cr source reference");
p.mass=xr(x,"mass_kg",p.mass);
p.drag=xr(x,"drag_area_m2",p.drag);
p.cd=xr(x,"cd",p.cd);
p.srp=xr(x,"srp_area_m2",p.srp);
p.cr=xr(x,"cr",p.cr);
p.source=attr(x,"source",p.source);
p.reference=attr(x,"reference",p.reference);
need(p.mass>0&&p.drag>=0&&p.cd>=0&&p.srp>=0&&p.cr>=0&&(p.source=="placeholder"||p.source=="documented")&&(p.source!="documented"||!p.reference.empty()),2,"phys","invalid physical parameters");
}
struct Event{
int status;
I from,to;
}
;
struct Sat{
uint32_t gid=0;
uint16_t layer=0,plane=0,slot=0;
uint32_t P=0,S=0;
int base=0;
std::string name,explicitname;
double a=RE+1200000,e=0,inc=87.9*pi/180,om=0,u=0,w=0;
Radio radio;
Phys phys;
std::vector<Event>events;
State y;
bool failed=false;
I failuretime=0;
std::string reason;
}
;
int status(const Sat&s,I t){
for(auto&e:s.events)if(e.from<=t&&t<e.to)return e.status;
return s.base;
}
int st(const std::string&s){
if(s=="ok")return 0;
if(s=="damaged")return 1;
if(s=="missing")return 2;
fail(2,"status",s);
}
void orbit(const X&x,Sat&s,bool explicitSat=false){
keys(x,explicitSat?"plane slot a_km alt_km e inc_deg raan_deg u_deg argp_deg name":"alt_km a_km e inc_deg raan0_deg u0_deg argp_deg");
need(!(x.a.count("a_km")&&x.a.count("alt_km")),2,"orbit","a/alt exclusive");
s.a=x.a.count("a_km")?xr(x,"a_km",0)*1000:RE+xr(x,"alt_km",1200)*1000;
s.e=xr(x,"e",0);
s.inc=xr(x,"inc_deg",87.9)*pi/180;
s.om=wrap(xr(x,explicitSat?"raan_deg":"raan0_deg",0)*pi/180);
s.u=wrap(xr(x,explicitSat?"u_deg":"u0_deg",0)*pi/180);
s.w=wrap(xr(x,"argp_deg",0)*pi/180);
need(s.a<=1e9&&s.e>=0&&s.e<1&&s.inc>=0&&s.inc<=pi&&s.a*(1-s.e)>=RE+120000,2,"orbit","invalid elements");
}
struct Config{
std::vector<Sat>s;
std::vector<uint32_t> physNotUsed;
B geometry,config;
std::string xml;
}
;
Config constellation(const fs::path&p,I start,I end,bool exm,bool exd,const std::map<uint32_t,PhysOv>*po=nullptr){
Config cfg;
cfg.xml=readtext(p);
cfg.config=sha(cfg.xml);
X root=XP(cfg.xml).parse();
need(root.tag=="constellation",2,"xml","constellation root");
keys(root,"name");
Radio rr;
Phys pp;
std::vector<X>ls;
bool rseen=false,pseen=false;
for(auto&x:root.c){
if(x.tag=="radio"){
need(!rseen,2,"xml","duplicate radio");
rseen=true;
radio(x,rr);
}
else if(x.tag=="phys"){
need(!pseen,2,"xml","duplicate phys");
pseen=true;
phys(x,pp);
}
else if(x.tag=="layer")ls.push_back(x);
else fail(2,"xml","unknown constellation child");
}
need(!ls.empty(),2,"xml","no layers");
std::set<U>ids;
for(auto&l:ls)if(l.a.count("id")){
U id=uint(l.a["id"]);
need(id>=1&&id<=65535&&ids.insert(id).second,2,"layer","id duplicate/range");
}
for(auto&l:ls)if(!l.a.count("id")){
U id=1;
while(ids.count(id))++id;
need(id<=65535,2,"layer","ids exhausted");
ids.insert(id);
l.a["id"]=std::to_string(id);
}
std::sort(ls.begin(),ls.end(),[](auto&a,auto&b){
return uint(a.a.at("id"))<uint(b.a.at("id"));}
);
B geom;
std::string magic="satpred-geometry-v2";
geom.insert(geom.end(),magic.begin(),magic.end());
geom.push_back(0);
append(geom,ls.size(),4);
U gid=0;
std::set<uint32_t> physSeen;
for(auto&l:ls){
keys(l,"id pattern P S F");
auto pat=attr(l,"pattern","star");
need(pat=="delta"||pat=="star"||pat=="explicit",2,"pattern","unsupported "+pat);
Radio r=rr;
Phys ph=pp;
Sat model;
std::vector<X>xs,ff;
bool os=false,rs=false,ps=false,fs=false;
for(auto&x:l.c){
if(x.tag=="orbit"){
need(!os&&pat!="explicit",2,"orbit","duplicate or explicit parent orbit");
os=true;
orbit(x,model);
}
else if(x.tag=="radio"){
need(!rs,2,"radio","duplicate");
rs=true;
radio(x,r);
}
else if(x.tag=="phys"){
need(!ps,2,"phys","duplicate");
ps=true;
phys(x,ph);
}
else if(x.tag=="faults"){
need(!fs,2,"faults","duplicate");
fs=true;
keys(x,"");
ff=x.c;
}
else if(x.tag=="satellite")xs.push_back(x);
else fail(2,"layer","unknown child "+x.tag);
}
U P=xu(l,"P",18),S=xu(l,"S",36),F=xu(l,"F",0);
std::vector<Sat> members;
if(pat=="explicit"){
need(!xs.empty(),2,"explicit","empty");
std::set<std::pair<U,U>>slots;
P=0;
S=0;
for(auto&x:xs){
Sat s;
orbit(x,s,true);
U p=xu(x,"plane",0),sl=0;
if(x.a.count("slot"))sl=uint(x.a.at("slot"));
else while(slots.count({
p,sl}
))++sl;
need(p<=65534&&sl<=65534&&slots.insert({
p,sl}
).second,2,"explicit","slot duplicate/range");
s.plane=uint16_t(p);
s.slot=uint16_t(sl);
s.explicitname=attr(x,"name");
s.radio=r;
s.phys=ph;
bool sr=false,sp=false;
for(auto&z:x.c){
if(z.tag=="radio"){
need(!sr,2,"radio","duplicate");
sr=true;
radio(z,s.radio);
}
else if(z.tag=="phys"){
need(!sp,2,"phys","duplicate");
sp=true;
phys(z,s.phys);
}
else fail(2,"satellite","unknown child");
}
members.push_back(s);
P=std::max(P,p+1);
S=std::max(S,sl+1);
}
need((!l.a.count("P")||uint(l.a.at("P"))==P)&&(!l.a.count("S")||uint(l.a.at("S"))==S),2,"explicit","P/S mismatch");
}
else{
need(P>=1&&P<=65535&&S>=1&&S<=65535&&mul(P,S)<=1000000&&F<P,2,"Walker","P/S/F range");
std::map<std::pair<U,U>,size_t> wlookup;
for(U p=0;p<P;++p)for(U sl=0;sl<S;++sl){
Sat s=model;
s.plane=uint16_t(p);
s.slot=uint16_t(sl);
s.om=wrap(model.om+(pat=="delta"?2*pi:pi)*double(p)/double(P));
s.u=wrap(model.u+2*pi*double(sl)/double(S)+2*pi*double(F*p)/double(P*S));
s.radio=r;
s.phys=ph;
wlookup[{p,sl}]=members.size();
members.push_back(s);
}
std::set<std::pair<U,U>> wslots;
for(auto&x:xs){
keys(x,"plane slot a_km alt_km e inc_deg raan_deg u_deg argp_deg name");
need(!(x.a.count("a_km")&&x.a.count("alt_km")),2,"satellite","a/alt exclusive");
U p=xu(x,"plane",0),sl=0;
if(x.a.count("slot"))sl=uint(x.a.at("slot"));
else while(wslots.count({p,sl}))++sl;
need(p<=65534&&sl<=65534&&wslots.insert({p,sl}).second,2,"satellite","slot duplicate/range");
need(p<P&&sl<S,2,"Walker","satellite override outside Walker grid");
Sat&s=members[wlookup.at({p,sl})];
if(x.a.count("a_km")||x.a.count("alt_km"))s.a=x.a.count("a_km")?real(x.a.at("a_km"))*1000:RE+real(x.a.at("alt_km"))*1000;
if(x.a.count("e"))s.e=real(x.a.at("e"));
if(x.a.count("inc_deg"))s.inc=real(x.a.at("inc_deg"))*pi/180;
if(x.a.count("raan_deg"))s.om=wrap(real(x.a.at("raan_deg"))*pi/180);
if(x.a.count("u_deg"))s.u=wrap(real(x.a.at("u_deg"))*pi/180);
if(x.a.count("argp_deg"))s.w=wrap(real(x.a.at("argp_deg"))*pi/180);
need(s.a<=1e9&&s.e>=0&&s.e<1&&s.inc>=0&&s.inc<=pi&&s.a*(1-s.e)>=RE+120000,2,"orbit","invalid elements");
s.explicitname=attr(x,"name");
bool sr=false,sp=false;
for(auto&z:x.c){
if(z.tag=="radio"){need(!sr,2,"radio","duplicate");sr=true;radio(z,s.radio);}
else if(z.tag=="phys"){need(!sp,2,"phys","duplicate");sp=true;phys(z,s.phys);}
else fail(2,"satellite","unknown child");
}
}
}
need(add(gid,mul(P,S))<=1000000,2,"catalog","nominal slots exceed 1e6");
std::sort(members.begin(),members.end(),[](auto&a,auto&b){
return std::tie(a.plane,a.slot)<std::tie(b.plane,b.slot);}
);
append(geom,uint(l.a.at("id")),2);
append(geom,pat=="delta"?0:pat=="star"?1:2,1);
append(geom,1,1);
append(geom,P,4);
append(geom,S,4);
append(geom,F,4);
if(pat=="explicit"){
append(geom,members.size(),4);
for(auto&s:members){
append(geom,s.plane,2);
append(geom,s.slot,2);
for(double v:{
s.a,s.e,s.inc,s.om,s.u,s.w}
)appendf(geom,v==0?0:v);
}
}
else for(double v:{
model.a,model.e,model.inc,model.om,model.u,model.w}
)appendf(geom,v==0?0:v);
std::map<std::pair<U,U>,size_t>lookup;
for(size_t j=0;j<members.size();++j)lookup[{
members[j].plane,members[j].slot}
]=j;
std::set<size_t>permanent;
for(auto&f:ff){
keys(f,f.tag=="event"?"plane slot status from to":"plane slot");
U p=xu(f,"plane",0),sl=xu(f,"slot",0);
auto it=lookup.find({
p,sl}
);
need(it!=lookup.end(),2,"faults","unknown member");
auto&s=members[it->second];
if(f.tag=="missing"||f.tag=="damaged"){
need(permanent.insert(it->second).second,2,"faults","duplicate permanent");
s.base=st(f.tag);
}
else if(f.tag=="event"){
I a=f.a.count("from")?utc(f.a.at("from")):start,b=f.a.count("to")?utc(f.a.at("to")):end;
need(a<b,2,"faults","empty/reversed interval");
s.events.push_back({
st(attr(f,"status","missing")),a,b}
);
}
else fail(2,"faults","unknown fault");
}
for(auto&s:members){
std::sort(s.events.begin(),s.events.end(),[](auto&a,auto&b){
return a.from<b.from;}
);
need(s.base!=2||s.events.empty(),2,"faults","permanent missing with events");
for(size_t i=1;i<s.events.size();++i)need(s.events[i-1].to<=s.events[i].from,2,"faults","overlap");
s.layer=uint16_t(uint(l.a.at("id")));
s.P=uint32_t(P);
s.S=uint32_t(S);
s.gid=uint32_t(gid+U(s.plane)*S+s.slot);
std::ostringstream n;
n<<'L'<<s.layer<<"-P"<<std::setfill('0')<<std::setw(2)<<s.plane<<"-S"<<std::setw(3)<<s.slot;
s.name=cut(s.explicitname.empty()?n.str():s.explicitname,24);
s.y=kepler(s.a,s.e,s.inc,s.om,s.u,s.w);
physSeen.insert(s.gid);
bool poUsed=false;
if(po){
auto it=po->find(s.gid);
if(it!=po->end()){
auto&o=it->second;
if(o.mass)s.phys.mass=o.mass_v;
if(o.drag)s.phys.drag=o.drag_v;
if(o.cd)s.phys.cd=o.cd_v;
if(o.srp)s.phys.srp=o.srp_v;
if(o.cr)s.phys.cr=o.cr_v;
if(o.source)s.phys.source=o.source_v;
if(o.reference)s.phys.reference=o.reference_v;
need(s.phys.mass>0&&s.phys.drag>=0&&s.phys.cd>=0&&s.phys.srp>=0&&s.phys.cr>=0&&(s.phys.source=="placeholder"||s.phys.source=="documented")&&(s.phys.source!="documented"||!s.phys.reference.empty()),2,"phys_json","invalid physical parameters");
poUsed=true;
}
}
bool excluded=(s.base==2&&exm)||(s.base==1&&exd);
if(poUsed&&excluded)cfg.physNotUsed.push_back(s.gid);
if(excluded)continue;
cfg.s.push_back(s);
}
gid+=P*S;
}
if(po)for(auto&kv:*po)need(physSeen.count(kv.first),2,"phys_json","gid "+std::to_string(kv.first)+" is not a nominal member");
need(!cfg.s.empty(),2,"catalog","all members excluded");
cfg.geometry=sha(geom);
return cfg;
}
struct Node{
uint32_t id=0;
int kind=0;
std::string name;
Radio radio;
V xyz,up;
}
;
std::vector<Node> grounds(const fs::path&p,size_t N){
X x=XP(readtext(p)).parse();
need(x.tag=="network",2,"nodes","network root");
keys(x,"name");
Radio u{
100000000,300000000,1}
,g{
10000000000ULL,10000000000ULL,4}
;
bool seen=false;
for(auto&z:x.c)if(z.tag=="defaults"){
need(!seen,2,"nodes","duplicate defaults");
seen=true;
keys(z,"");
std::set<std::string>ss;
for(auto&t:z.c){
need(ss.insert(t.tag).second,2,"defaults","duplicate");
if(t.tag=="user")radio(t,u);
else if(t.tag=="ground_station")radio(t,g);
else fail(2,"defaults","unknown");
}
}
std::set<std::string>ids;
for(auto&z:x.c)if(z.tag!="defaults"&&z.a.count("id"))need(!z.a.at("id").empty()&&ids.insert(z.a.at("id")).second,2,"nodes","duplicate/empty id");
std::vector<Node>out;
for(auto&z:x.c){
if(z.tag=="defaults")continue;
need(z.tag=="user"||z.tag=="ground_station",2,"nodes","unknown node");
keys(z,"id lat_deg lon_deg alt_m uplink_bps downlink_bps antennas");
Node n;
n.kind=z.tag=="user"?1:2;
n.id=uint32_t(N+out.size());
n.radio=n.kind==1?u:g;
n.radio.up=xu(z,"uplink_bps",n.radio.up);
n.radio.down=xu(z,"downlink_bps",n.radio.down);
U ant=xu(z,"antennas",n.radio.ant);
need(ant<=UINT32_MAX,2,"nodes","antenna overflow");
n.radio.ant=uint32_t(ant);
n.name=attr(z,"id");
if(n.name.empty()){
U j=1;
std::string prefix=n.kind==1?"u":"gw";
while(ids.count(prefix+std::to_string(j)))++j;
n.name=prefix+std::to_string(j);
ids.insert(n.name);
}
double lat=xr(z,"lat_deg",0),lon=xr(z,"lon_deg",0),h=xr(z,"alt_m",0);
need(lat>=-90&&lat<=90&&lon>=-180&&lon<=180&&h>=0,2,"nodes","geodetic range");
lat*=pi/180;
lon*=pi/180;
double e2=1-RB*RB/(RE*RE),v=RE/std::sqrt(1-e2*std::sin(lat)*std::sin(lat));
n.up={
std::cos(lat)*std::cos(lon),std::cos(lat)*std::sin(lon),std::sin(lat)}
;
n.xyz={
(v+h)*n.up.x,(v+h)*n.up.y,((1-e2)*v+h)*n.up.z}
;
out.push_back(n);
}
return out;
}
struct Settings{
bool drag=true,third=true,srp=true,rel=true;
double scale=1,dut=0,xp=0,yp=0,tt=69.184,j2=1.08262617385222e-3,j3=-2.53241051856772e-6,j4=-1.61989759991697e-6;
std::string raw;
std::vector<I> eopT,ephT,denT;
std::vector<std::array<double,4>> eopV;
std::vector<std::array<double,6>> ephV;
std::vector<double> denV;
std::map<uint32_t,PhysOv> phys;
struct Ext{std::string role,sha;U size;};
std::vector<Ext> ext;
}
;
std::vector<std::string> csvsplit(const std::string&line){
std::vector<std::string> v;
size_t i=0;
for(;;){
size_t e=line.find(',',i);
if(e==std::string::npos){v.push_back(line.substr(i));break;}
v.push_back(line.substr(i,e-i));
i=e+1;
}
return v;
}
std::vector<std::vector<std::string>> csvrows(const std::string&raw,const std::string&role,const std::string&cols){
size_t i=0;
if(raw.size()>=3&&uint8_t(raw[0])==0xEF&&uint8_t(raw[1])==0xBB&&uint8_t(raw[2])==0xBF)i=3;
std::vector<std::vector<std::string>> rows;
while(i<raw.size()){
size_t e=raw.find('\n',i);
if(e==std::string::npos)e=raw.size();
std::string line=raw.substr(i,e-i);
if(!line.empty()&&line.back()=='\r')line.pop_back();
need(!line.empty(),2,role,"empty row/comment not allowed");
rows.push_back(csvsplit(line));
i=e+1;
}
need(rows.size()>=3,2,role,"need header and >=2 data rows");
need(rows[0]==csvsplit(cols),2,role,"unexpected column names");
return rows;
}
std::string extread(const fs::path&p,const std::string&role,Settings&s){
std::string raw=readtext(p);
B b(raw.begin(),raw.end());
s.ext.push_back({role,hex(sha(b)),U(b.size())});
return raw;
}
Settings settings(const fs::path&p){
Settings s;
if(p=="none")return s;
s.raw=readtext(p);
J j=json(s.raw);
need(j.type==J::Obj,2,"settings","object");
for(auto&kv:j.o){
auto&k=kv.first;
auto&v=kv.second;
if(k=="drag")s.drag=v.flag();
else if(k=="third_bodies")s.third=v.flag();
else if(k=="srp")s.srp=v.flag();
else if(k=="relativity")s.rel=v.flag();
else if(k=="density_scale")s.scale=v.real();
else if(k=="dut1_s")s.dut=v.real();
else if(k=="xp_arcsec")s.xp=v.real();
else if(k=="yp_arcsec")s.yp=v.real();
else if(k=="tt_minus_utc_s")s.tt=v.real();
else if(k=="gravity_json"){
auto q=json(extread(p.parent_path()/fs::u8path(v.str()),"gravity",s));
need(q.o.size()==1&&q.o.count("J"),2,"gravity","J only");
for(auto&a:q.at("J").o){
if(a.first=="2")s.j2=a.second.real();
else if(a.first=="3")s.j3=a.second.real();
else if(a.first=="4")s.j4=a.second.real();
else fail(2,"gravity","order");
}
}
else if(k=="eop_csv"){
auto rows=csvrows(extread(p.parent_path()/fs::u8path(v.str()),"eop",s),"eop","utc_seconds,dut1_s,xp_arcsec,yp_arcsec,tt_minus_utc_s");
for(size_t i=1;i<rows.size();++i){
need(rows[i].size()==5,2,"eop","column count");
I t=seconds(rows[i][0]);
std::array<double,4> a{real(rows[i][1]),real(rows[i][2]),real(rows[i][3]),real(rows[i][4])};
need(s.eopT.empty()||t>s.eopT.back(),2,"eop","utc_seconds not strictly increasing");
s.eopT.push_back(t);s.eopV.push_back(a);
}
}
else if(k=="ephemeris_csv"){
auto rows=csvrows(extread(p.parent_path()/fs::u8path(v.str()),"ephemeris",s),"ephemeris","utc_seconds,sun_x,sun_y,sun_z,moon_x,moon_y,moon_z");
for(size_t i=1;i<rows.size();++i){
need(rows[i].size()==7,2,"ephemeris","column count");
I t=seconds(rows[i][0]);
std::array<double,6> a;
for(int c=0;c<6;++c)a[size_t(c)]=real(rows[i][size_t(c)+1]);
need(s.ephT.empty()||t>s.ephT.back(),2,"ephemeris","utc_seconds not strictly increasing");
s.ephT.push_back(t);s.ephV.push_back(a);
}
}
else if(k=="density_csv"){
auto rows=csvrows(extread(p.parent_path()/fs::u8path(v.str()),"density",s),"density","utc_seconds,rho_kg_m3");
for(size_t i=1;i<rows.size();++i){
need(rows[i].size()==2,2,"density","column count");
I t=seconds(rows[i][0]);
double rho=real(rows[i][1]);
need(rho>=0,2,"density","negative rho");
need(s.denT.empty()||t>s.denT.back(),2,"density","utc_seconds not strictly increasing");
s.denT.push_back(t);s.denV.push_back(rho);
}
}
else if(k=="phys_json"){
J q=json(extread(p.parent_path()/fs::u8path(v.str()),"phys",s));
need(q.type==J::Obj,2,"phys_json","object");
for(auto&e:q.o){
need(e.second.type==J::Obj&&!e.second.o.empty(),2,"phys_json","empty or non-object slot");
U gid=uint(e.first);
need(gid<=UINT32_MAX&&std::to_string(gid)==e.first,2,"phys_json","gid key");
PhysOv o;
for(auto&a:e.second.o){
auto&kk=a.first;auto&vv=a.second;
if(kk=="mass_kg"){o.mass=true;o.mass_v=vv.real();}
else if(kk=="drag_area_m2"){o.drag=true;o.drag_v=vv.real();}
else if(kk=="cd"){o.cd=true;o.cd_v=vv.real();}
else if(kk=="srp_area_m2"){o.srp=true;o.srp_v=vv.real();}
else if(kk=="cr"){o.cr=true;o.cr_v=vv.real();}
else if(kk=="source"){o.source=true;o.source_v=vv.str();}
else if(kk=="reference"){o.reference=true;o.reference_v=vv.str();}
else fail(2,"phys_json","unknown key "+kk);
}
need(!s.phys.count(uint32_t(gid)),2,"phys_json","duplicate gid");
s.phys[uint32_t(gid)]=o;
}
}
else fail(2,"settings","unknown "+k);
}
need(s.scale>=0,2,"settings","negative density");
return s;
}
std::array<double,4> eopAt(const Settings&s,double U){
if(s.eopT.empty())return{s.dut,s.xp,s.yp,s.tt};
double x=U*1e9;
need(x>=double(s.eopT.front())&&x<=double(s.eopT.back()),2,"eop_csv","evaluation time outside EOP table (no extrapolation)");
size_t k=0;
while(k+1<s.eopT.size()&&double(s.eopT[k+1])<=x)++k;
std::array<double,4> z=s.eopV[k];
if(k+1<s.eopT.size()){
double f=(x-double(s.eopT[k]))/(double(s.eopT[k+1])-double(s.eopT[k]));
for(int i=0;i<4;++i)z[size_t(i)]+=(s.eopV[k+1][size_t(i)]-s.eopV[k][size_t(i)])*f;
}
return z;
}
std::pair<V,V> ephAt(const Settings&s,double U){
double x=U*1e9;
need(x>=double(s.ephT.front())&&x<=double(s.ephT.back()),2,"ephemeris_csv","evaluation time outside ephemeris table (no extrapolation)");
size_t k=0;
while(k+1<s.ephT.size()&&double(s.ephT[k+1])<=x)++k;
std::array<double,6> z=s.ephV[k];
if(k+1<s.ephT.size()){
double f=(x-double(s.ephT[k]))/(double(s.ephT[k+1])-double(s.ephT[k]));
for(int i=0;i<6;++i)z[size_t(i)]+=(s.ephV[k+1][size_t(i)]-s.ephV[k][size_t(i)])*f;
}
return{V{z[0],z[1],z[2]},V{z[3],z[4],z[5]}};
}
double denAt(const Settings&s,double U){
double x=U*1e9;
need(x>=double(s.denT.front())&&x<=double(s.denT.back()),2,"density_csv","evaluation time outside density table (no extrapolation)");
size_t k=0;
while(k+1<s.denT.size()&&double(s.denT[k+1])<=x)++k;
if(k+1>=s.denT.size())return s.denV.back();
double f=(x-double(s.denT[k]))/(double(s.denT[k+1])-double(s.denT[k]));
return s.denV[k]+(s.denV[k+1]-s.denV[k])*f;
}
Mat precession(double U,const Settings&s){
double T=(U+eopAt(s,U)[3]-946728000)/(36525.0*86400),arc=pi/648000;
return rz((2306.2181*T+1.09468*T*T+.018203*T*T*T)*arc)*ry(-(2004.3109*T-.42665*T*T-.041833*T*T*T)*arc)*rz((2306.2181*T+.30188*T*T+.017998*T*T*T)*arc);
}
Mat rotation(double U,const Settings&s){
auto e=eopAt(s,U);
double d=(U+e[0]-946728000)/86400,T=d/36525,gm=(280.46061837+360.98564736629*d+.000387933*T*T-T*T*T/38710000)*pi/180;
return ry(e[1]*pi/648000)*rx(e[2]*pi/648000)*rz(-wrap(gm))*precession(U,s);
}
V ellipse(double a,double e,double M){
M=wrap(M);
double lo=0,hi=2*pi;
for(int j=0;j<60;++j){
double E=(lo+hi)/2;
if(E-e*std::sin(E)<M)lo=E;
else hi=E;
}
double E=(lo+hi)/2;
return{
a*(std::cos(E)-e),a*std::sqrt(1-e*e)*std::sin(E),0}
;
}
std::pair<V,V>sunmoon(double U,const Settings&s){
if(!s.ephT.empty())return ephAt(s,U);
double d=(U-946598400)/86400,deg=pi/180,ws=(282.9404+4.70935e-5*d)*deg,Ms=wrap((356.0470+.9856002585*d)*deg);
V sun=rz(ws)*ellipse(AU,.016709-1.151e-9*d,Ms);
double om=(125.1228-.0529538083*d)*deg,i=5.1454*deg,w=(318.0634+.1643573223*d)*deg,M=wrap((115.3654+13.0649929509*d)*deg);
V moon=rz(om)*rx(i)*rz(w)*ellipse(60.2666*6378140,.0549,M);
double lon=std::atan2(moon.y,moon.x),lat=std::asin(moon.z/norm(moon)),dist=norm(moon),D=M+w+om-(Ms+ws),F=M+w;
double dl=-1.274*std::sin(M-2*D)+.658*std::sin(2*D)-.186*std::sin(Ms)-.059*std::sin(2*M-2*D)-.057*std::sin(M-2*D+Ms)+.053*std::sin(M+2*D)+.046*std::sin(2*D-Ms)+.041*std::sin(M-Ms)-.035*std::sin(D)-.031*std::sin(M+Ms)-.015*std::sin(2*F-2*D)+.011*std::sin(M-4*D);
double db=-.173*std::sin(F-2*D)-.055*std::sin(M-F-2*D)-.046*std::sin(M+F-2*D)+.033*std::sin(F+2*D)+.017*std::sin(2*M+F);
lon+=dl*deg;
lat+=db*deg;
dist+=6378140*(-.58*std::cos(M-2*D)-.46*std::cos(2*D));
moon={
dist*std::cos(lat)*std::cos(lon),dist*std::cos(lat)*std::sin(lon),dist*std::sin(lat)}
;
Mat Q=precession(U,s).tr()*rx((23.4393-3.563e-7*d)*deg);
return{
Q*sun,Q*moon}
;
}
struct Env{
Mat R,D;
V sun,moon;
double rho=-1;
}
;
struct Environment{
I start,ens;
Settings s;
mutable I cached=-1;
mutable Env a,b;
double orthmax=0;
Env node(I index)const{
double U=double(start/1000000000)+double(start%1000000000)*1e-9+double(index)*double(ens)*1e-9;
need(U>=0&&U<4133980800.0,2,"environment","built-in ephemeris date outside 1970..2100");
Env e;
e.R=rotation(U,s);
Mat p=rotation(U+.5,s),m=rotation(U-.5,s);
for(int i=0;i<9;++i)e.D.a[i]=p.a[i]-m.a[i];
auto sm=sunmoon(U,s);
e.sun=sm.first;
e.moon=sm.second;
e.rho=s.denT.empty()?-1.0:denAt(s,U);
return e;
}
Env at(double rel)const{
double z=rel/(double(ens)*1e-9);
I j=I(std::floor(z));
if(cached!=j){
a=node(j);
b=node(j+1);
cached=j;
}
double f=z-double(j);
Env e;
for(int k=0;k<9;++k){
e.R.a[k]=a.R.a[k]+f*(b.R.a[k]-a.R.a[k]);
e.D.a[k]=a.D.a[k]+f*(b.D.a[k]-a.D.a[k]);
}
e.sun=a.sun+(b.sun-a.sun)*f;
e.moon=a.moon+(b.moon-a.moon)*f;
e.rho=a.rho+f*(b.rho-a.rho);
return e;
}
}
;
double density(double h){
static constexpr double hs[]={
120000,150000,200000,250000,300000,400000,500000,600000,700000,800000,1000000,1500000}
,rs[]={
2e-8,2e-9,3e-10,7e-11,2e-11,4e-12,8e-13,2e-13,7e-14,3e-14,5e-15,1e-16}
;
if(h<=hs[0])return rs[0];
for(int j=1;j<12;++j)if(h<=hs[j])return std::exp(std::log(rs[j-1])+(h-hs[j-1])/(hs[j]-hs[j-1])*(std::log(rs[j])-std::log(rs[j-1])));
return rs[11];
}
State derivative(const State&y,const Phys&p,const Settings&s,const Env&e){
V r=y.r(),v=y.v(),rf=e.R*r;
double n=norm(rf),z=rf.z/n;
V af=rf*(-mu/(n*n*n));
double P0=1,P1=z,D0=0,D1=1;
double J[]={
0,0,s.j2,s.j3,s.j4}
;
for(int k=2;k<=4;++k){
double P=((2*k-1)*z*P1-(k-1)*P0)/k,D=((2*k-1)*(P1+z*D1)-(k-1)*D0)/k,f=mu*J[k]*std::pow(RE,k)/std::pow(n,k+3),g=(k+1)*P+z*D;
af=af+V{
f*rf.x*g,f*rf.y*g,f*n*((k+1)*z*P-(1-z*z)*D)}
;
P0=P1;
P1=P;
D0=D1;
D1=D;
}
V a=e.R.tr()*af;
n=norm(r);
if(s.third){
auto third=[&](V R,double m){
V d=R-r;
double dn=norm(d),rn=norm(R);
return(d*(1/(dn*dn*dn))-R*(1/(rn*rn*rn)))*m;
}
;
a=a+third(e.sun,1.32712440018e20)+third(e.moon,4.902800066e12);
}
if(s.rel)a=a+(r*(4*mu/n-dot(v,v))+v*(4*dot(r,v)))*(mu/(299792458.0*299792458.0*n*n*n));
if(s.drag){
V vr=e.R*v+e.D*r;
double rho=e.rho>=0?e.rho:density(n-RE);
a=a+e.R.tr()*(vr*(-.5*rho*s.scale*p.cd*p.drag/p.mass*norm(vr)));
}
if(s.srp){
V u=e.sun*(1/norm(e.sun));
bool shadow=dot(r,u)<0&&norm(r-u*dot(r,u))<RE;
if(!shadow){
V d=r-e.sun;
double dn=norm(d);
a=a+d*(4.56e-6*p.cr*p.srp/p.mass*(AU*AU/(dn*dn))/dn);
}
}
return state(v,a);
}
bool valid(const State&y){
for(auto v:y.q)if(!std::isfinite(v))return false;
return norm(y.r())>RE+100000;
}
double qnan(){
U v=0x7ff8000000000000ULL;
double x;
std::memcpy(&x,&v,8);
return x;
}
void advance(std::vector<Sat>&s,U k,I step,I maxh,Environment&env){
U sub=(U(step)+U(maxh)-1)/U(maxh);
need(sub<=10000000,2,"integrator","substep count resource limit");
double h=double(step)*1e-9/double(sub);
for(U j=0;j<sub;++j){
safe();
double t=double(k)*double(step)*1e-9+double(j)*h;
std::array<Env,12>es;
for(int stage=0;stage<12;++stage)es[stage]=env.at(t+rk::C[stage]*h);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
for(I ix=0;ix<I(s.size());++ix){
auto&a=s[size_t(ix)];
if(a.failed)continue;
std::array<State,12>K;
State y1=a.y;
int bad=-1;
for(int stage=0;stage<12;++stage){
State y=a.y;
for(int c=0;c<6;++c){
double v=0;
for(int m=0;m<stage;++m)v+=rk::A[stage][m]*K[m].q[c];
y.q[c]+=h*v;
}
if(!valid(y)){
bad=stage;
break;
}
K[stage]=derivative(y,a.phys,env.s,es[stage]);
for(auto v:K[stage].q)if(!std::isfinite(v))bad=stage;
if(bad>=0)break;
}
if(bad<0){
for(int c=0;c<6;++c){
double v=0;
for(int stage=0;stage<12;++stage)v+=rk::B[stage]*K[stage].q[c];
y1.q[c]+=h*v;
}
if(!valid(y1))bad=12;
}
if(bad>=0){
a.failed=true;
a.failuretime=env.start+I(std::nearbyint((t+(bad==12?1:rk::C[bad])*h)*1e9));
a.reason="nonfinite_or_radius_below_100km";
a.y.q.fill(qnan());
}
else a.y=y1;
}
}
}
fs::path executable(){
#ifdef _WIN32
std::wstring s(32768,L'\0');
DWORD n=GetModuleFileNameW(nullptr,s.data(),DWORD(s.size()));
need(n>0&&n<s.size(),3,"paths","GetModuleFileNameW");
s.resize(n);
return fs::path(s);
#else
std::array<char,8192>b{
}
;
auto n=readlink("/proc/self/exe",b.data(),b.size());
need(n>0&&size_t(n)<b.size(),3,"paths","/proc/self/exe");
return fs::path(std::string(b.data(),size_t(n)));
#endif
}
fs::path home(){
auto p=std::getenv("SATPRED_HOME");
if(p)return fs::u8path(p);
#ifdef _WIN32
p=std::getenv("USERPROFILE");
#else
p=std::getenv("HOME");
#endif
need(p,3,"home","home environment unavailable");
return fs::u8path(p)/".satpred";
}
fs::path newrun(const std::string&out){
if(out!="auto"){
fs::path p=fs::absolute(fs::u8path(out));
need(!fs::exists(p)||fs::is_empty(p),1,"output","directory exists and is not empty");
fs::create_directories(p);
return p;
}
auto h=home()/"runs";
fs::create_directories(h);
for(U i=1;;++i){
std::ostringstream n;
n<<"run_"<<std::setfill('0')<<std::setw(6)<<i;
auto p=h/n.str();
std::error_code ec;
if(fs::create_directory(p,ec))return p;
need(!ec,3,"output",ec.message());
}
}
struct Options{
std::map<std::string,std::string>m;
std::set<std::string>explicitkeys;
}
;
std::map<std::string,std::string>defaults(const std::string&app){
auto bin=executable().parent_path();
std::map<std::string,std::string>m={
{
"start","2026-01-01T00:00:00Z"}
,{
"duration","86400"}
,{
"step","10"}
,{
"package","3600"}
,{
"threads","2"}
,{
"memory-budget","268435456"}
,{
"output","auto"}
}
;
if(app=="satpred"){
m.insert({
{
"config",pathtext((bin/"examples/satellites.xml"))}
,{
"settings",pathtext((bin/"examples/settings.json"))}
,{
"max-h","10"}
,{
"env-step","1"}
,{
"batch","64"}
,{
"frame","eci"}
,{
"initial-npy","none"}
,{
"exclude-missing","false"}
,{
"exclude-damaged","false"}
}
);
}
else{
m.insert({
{
"satellites",pathtext((bin/"examples/satellites.xml"))}
,{
"nodes",pathtext((bin/"examples/nodes.xml"))}
,{
"orbit-program",pathtext((bin/"satpred"))}
,{
"orbit-dir","none"}
,{
"topology","mesh"}
,{
"backend","cpu"}
,{
"search-range-km","5000"}
,{
"access-range-km","3000"}
,{
"minimum-elevation","25"}
,{
"knn-k","4"}
,{
"wrap-planes","false"}
,{
"access-first","false"}
,{
"exclude-damaged","false"}
,{
"reference-distance-km","1000"}
,{
"distance-exponent","2"}
,{
"processing-us","0"}
}
);
#ifdef _WIN32
m["orbit-program"]+=".exe";
#endif
}
return m;
}
Options options(const std::string&app,int argc,char**argv){
Options o;
o.m=defaults(app);
for(int i=1;i<argc;++i){
std::string k=argv[i],v;
if(k=="-c")k="--config";
need(k.rfind("--",0)==0,1,"cli","expected --name");
k.erase(0,2);
auto e=k.find('=');
if(e!=std::string::npos){
v=k.substr(e+1);
k.resize(e);
}
need(o.m.count(k)||k=="help"||k=="version"||k=="dump-defaults"||k=="selftest",1,"cli","unknown "+k);
need(o.explicitkeys.insert(k).second,1,"cli","duplicate "+k);
bool flag=k=="help"||k=="version"||k=="dump-defaults"||k=="selftest"||k=="exclude-missing"||k=="exclude-damaged"||k=="wrap-planes"||k=="access-first";
if(flag){
need(e==std::string::npos,1,"cli","boolean flag takes no value");
v="true";
}
else if(e==std::string::npos){
need(i+1<argc,1,"cli","missing value");
v=argv[++i];
}
o.m[k]=v;
}
for(auto k:{
"help","version","dump-defaults","selftest"}
)if(o.explicitkeys.count(k))need(o.explicitkeys.size()==1,1,"cli","output-only mode exclusive");
return o;
}
J effective(const Options&o){
J j=J::obj();
for(auto&kv:o.m)j[kv.first]=kv.second;
return j;
}
struct Run{
I start,end,step,package,maxh=10000000000LL,ens=1000000000;
U M,full,files,budget;
int threads;
}
;
Run runtime(const Options&o){
Run r;
r.start=utc(o.m.at("start"));
I d=seconds(o.m.at("duration"));
r.step=seconds(o.m.at("step"));
r.package=seconds(o.m.at("package"));
need(d>0&&d<=2592000000000000LL&&r.step>=1000000&&r.package>=r.step&&r.package%r.step==0,1,"time","invalid duration/step/package");
need(d<=INT64_MAX-r.start,1,"time","end overflow");
r.end=r.start+d;
r.M=(U(d)+U(r.step)-1)/U(r.step);
r.full=U(r.package/r.step);
r.files=(r.M+r.full-1)/r.full;
need(r.full<=UINT32_MAX&&r.files<=UINT32_MAX,1,"time","package count exceeds format");
r.budget=uint(o.m.at("memory-budget"));
U t=uint(o.m.at("threads"));
need(r.budget>0&&t>0&&t<=INT_MAX,1,"runtime","threads/budget range");
r.threads=int(t);
if(o.m.count("max-h")){
r.maxh=seconds(o.m.at("max-h"));
r.ens=seconds(o.m.at("env-step"));
need(r.maxh>=1&&r.ens>=1000000,1,"time","max-h/env-step range");
need(uint(o.m.at("batch"))>=1,1,"batch","zero");
}
#ifdef _OPENMP
omp_set_num_threads(r.threads);
#endif
return r;
}
void child(const fs::path&prog,const std::vector<std::string>&args){
safe();
#ifdef _WIN32
auto wide=[](const std::string&s){
auto p=fs::u8path(s);
return p.wstring();
}
;
auto quoting=[](const std::wstring&s){
std::wstring r=L"\"";
size_t n=0;
for(auto c:s){
if(c==L'\\'){
++n;
continue;
}
if(c==L'\"'){
r.append(n*2+1,L'\\');
r+=c;
}
else{
r.append(n,L'\\');
r+=c;
}
n=0;
}
r.append(n*2,L'\\');
return r+L"\"";
}
;
std::wstring cmd=quoting(prog.wstring());
for(auto&a:args)cmd+=L" "+quoting(wide(a));
STARTUPINFOW si{
}
;
si.cb=sizeof(si);
PROCESS_INFORMATION pi{
}
;
need(CreateProcessW(prog.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)!=0,3,"process","CreateProcessW failed");
WaitForSingleObject(pi.hProcess,INFINITE);
DWORD code;
GetExitCodeProcess(pi.hProcess,&code);
CloseHandle(pi.hThread);
CloseHandle(pi.hProcess);
int c=int(code);
#else
pid_t pid=fork();
need(pid>=0,3,"process","fork failed");
if(pid==0){
std::vector<std::string>s;
s.push_back(pathtext(prog));
s.insert(s.end(),args.begin(),args.end());
std::vector<char*>p;
for(auto&a:s)p.push_back(a.data());
p.push_back(nullptr);
execv(prog.c_str(),p.data());
_exit(127);
}
int status=0;
while(waitpid(pid,&status,0)<0){
if(errno==EINTR)continue;
fail(3,"process","waitpid failed");
}
int c=WIFEXITED(status)?WEXITSTATUS(status):3;
std::string detail;
if(WIFSIGNALED(status))detail=" terminated by signal "+std::to_string(WTERMSIG(status));
else if(c==127)detail=" (exec failed: program missing or not executable)";
#endif
#ifdef _WIN32
std::string detail;
#endif
if(c){
fail(c>=1&&c<=7?c:(c==127?3:5),"process","child failed: "+pathtext(prog)+" exit "+std::to_string(c)+detail+"; the first error line above is from the child");
}
safe();
}
struct Section{
uint32_t type;
B data;
U offset=0;
}
;
struct Orbit{
B bytes;
U N,Ns,M,k0,tensor,stride;
I t0,step,start,end;
uint8_t frame;
std::vector<Sat>s;
std::vector<Section>sec;
State at(U j,U i)const{
State y;
for(int c=0;c<6;++c)y.q[c]=gf(bytes,tensor+j*stride+U(c)*Ns*8+i*8);
return y;
}
}
;
B sat_table(const std::vector<Sat>&s,U Ns,I t){
B b(size_t(Ns*12));
for(U i=0;i<Ns;++i){
put(b,i*4,i<s.size()?s[size_t(i)].gid:UINT32_MAX,4);
if(i>=s.size())continue;
auto&a=s[size_t(i)];
put(b,4*Ns+i*2,a.layer,2);
put(b,6*Ns+i*2,a.plane,2);
put(b,8*Ns+i*2,a.slot,2);
put(b,10*Ns+i,status(a,t),1);
}
return b;
}
std::vector<Event>segments(const Sat&s,I a,I b){
std::vector<I>p={
a,b}
;
for(auto&e:s.events){
if(e.from>a&&e.from<b)p.push_back(e.from);
if(e.to>a&&e.to<b)p.push_back(e.to);
}
std::sort(p.begin(),p.end());
p.erase(std::unique(p.begin(),p.end()),p.end());
std::vector<Event>v;
for(size_t i=0;i+1<p.size();++i){
int st=status(s,p[i]);
if(!v.empty()&&v.back().status==st)v.back().to=p[i+1];
else v.push_back({
st,p[i],p[i+1]}
);
}
return v;
}
B writeorbit(Config&cfg,const Run&r,U fi,Environment&env,const B&runid){
U k0=fi*r.full,M=std::min(r.full,r.M-k0),Ns=align(cfg.s.size(),64);
I t0=r.start+I(k0)*r.step,pend=std::min(r.end,r.start+I(k0+M)*r.step);
std::vector<Section>sec;
sec.push_back({
1,sat_table(cfg.s,Ns,t0)}
);
B names(size_t(Ns*24));
for(size_t i=0;i<cfg.s.size();++i)ps(names,i*24,cfg.s[i].name,24);
sec.push_back({
2,std::move(names)}
);
sec.push_back({
3,B(size_t(align(M*8,64)+M*4))}
);
B events(8);
U ec=0;
bool missing=false;
for(size_t i=0;i<cfg.s.size();++i){
auto&s=cfg.s[i];
if(s.base==2)missing=true;
for(auto&e:s.events)if(e.status==2)missing=true;
for(auto&e:segments(s,t0,pend))if(e.status!=status(s,t0)){
append(events,i,4);
append(events,e.status,1);
append(events,0,3);
append(events,U(e.from),8);
append(events,U(e.to),8);
++ec;
}
}
if(ec){
put(events,0,ec,4);
sec.push_back({
4,std::move(events)}
);
}
U stride=Ns*48;
need(add(mul(M,stride),mul(cfg.s.size(),800))<r.budget/2,2,"memory","MVP uses package buffer; shorten package or increase budget");
sec.push_back({
16,B(size_t(mul(M,stride)))}
);
for(U j=0;j<M;++j){
safe();
U k=k0+j;
if(k>0)advance(cfg.s,k-1,r.step,r.maxh,env);
I t=r.start+I(k)*r.step;
Env e=env.at(double(k)*double(r.step)*1e-9);
Mat Rt=e.R.tr()*e.R;
for(int a=0;a<9;++a)env.orthmax=std::max(env.orthmax,std::abs(Rt.a[a]-(a%4==0?1:0)));
auto&tensor=sec.back().data;
for(U i=0;i<Ns;++i){
State y;
y.q.fill(qnan());
if(i<cfg.s.size()){
auto&s=cfg.s[size_t(i)];
if(status(s,t)!=2&&!s.failed){
y=s.y;
}
}
for(int c=0;c<6;++c)pf(tensor,j*stride+U(c)*Ns*8+i*8,y.q[c]);
}
// Transform after nominal values have been written, preserving canonical NaNs.
put(sec[2].data,j*8,U(t),8);
}
U offset=512+sec.size()*32;
for(auto&s:sec){
offset=align(offset,s.type==16?4096:64);
s.offset=offset;
offset=add(offset,s.data.size());
}
B b(size_t(offset+32));
ps(b,0,"SATCONT3",8);
put(b,8,3,2);
put(b,12,512,4);
put(b,16,1,1);
put(b,17,1,1);
put(b,18,1,1);
put(b,19,2,1);
put(b,20,sec.size(),4);
put(b,24,512,8);
put(b,32,cfg.s.size(),4);
put(b,36,Ns,4);
put(b,40,M,4);
put(b,44,r.full,4);
put(b,48,U(t0),8);
put(b,56,U(r.step),8);
put(b,64,U(r.start),8);
put(b,72,U(r.end),8);
put(b,80,k0,8);
put(b,88,fi,4);
put(b,92,r.files,4);
put(b,97,6,1);
put(b,98,63,1);
put(b,100,(missing?1:0)|(ec?2:0),4);
put(b,120,Ns*8,8);
put(b,128,stride,8);
put(b,136,U(r.package),8);
put(b,144,get(cfg.geometry,0,8),8);
put(b,152,get(cfg.config,0,8),8);
std::copy(runid.begin(),runid.end(),b.begin()+168);
ps(b,200,"c-rk8",16);
ps(b,216,"cpu",16);
ps(b,232,"UTC-POSIX",16);
ps(b,248,iso(t0),32);
put(b,280,1,4);
put(b,284,65536,4);
for(size_t i=0;i<sec.size();++i){
auto&s=sec[i];
U d=512+i*32;
put(b,d,s.type,4);
put(b,d+4,s.type==16?12:6,2);
put(b,d+8,s.offset,8);
put(b,d+16,s.data.size(),8);
if(s.type!=16)put(b,d+24,crc(s.data),4);
std::copy(s.data.begin(),s.data.end(),b.begin()+I(s.offset));
}
ps(b,offset,"SEND",4);
put(b,offset+8,b.size(),8);
put(b,offset+16,sec.back().data.size(),8);
return b;
}
void finalizeorbit(B&b,Environment&env,bool ecef){
U M=get(b,40,4),Ns=get(b,36,4),N=get(b,32,4),stride=get(b,128,8),sc=get(b,20,4),ten=0,ti=0,tis=0,tidir=0;
for(U i=0;i<sc;++i){
U d=512+i*32,typ=get(b,d,4);
if(typ==16)ten=get(b,d+8,8);
if(typ==3){
ti=get(b,d+8,8);
tis=get(b,d+16,8);
tidir=d;
}
}
I t0=I(get(b,48,8));
if(ecef){
put(b,18,0,1);
for(U j=0;j<M;++j){
double rel=double(t0-env.start)*1e-9+double(j)*double(I(get(b,56,8)))*1e-9;
Env e=env.at(rel);
for(U i=0;i<N;++i){
State y;
for(int c=0;c<6;++c)y.q[c]=gf(b,ten+j*stride+U(c)*Ns*8+i*8);
if(!std::isfinite(y.q[0]))continue;
State z=state(e.R*y.r(),e.R*y.v()+e.D*y.r());
for(int c=0;c<6;++c)pf(b,ten+j*stride+U(c)*Ns*8+i*8,z.q[c]);
}
}
}
for(U j=0;j<M;++j)put(b,ti+align(M*8,64)+j*4,crc(b.data()+ten+j*stride,size_t(stride)),4);
put(b,tidir+24,crc(b.data()+ti,size_t(tis)),4);
put(b,b.size()-28,crc(b.data()+512,size_t(sc*32)),4);
put(b,508,crc(b.data(),508),4);
}
Orbit readorbit(const fs::path&p,U budget){
Orbit o;
o.bytes=load(p,budget);
auto&b=o.bytes;
need(b.size()>=544&&gs(b,0,8)=="SATCONT3"&&get(b,8,2)==3&&get(b,12,4)==512&&get(b,16,1)==1&&get(b,508,4)==crc(b.data(),508),7,"SATC","header");
need(get(b,17,1)==1&&get(b,18,1)<=2&&get(b,19,1)==2&&get(b,96,1)==0&&get(b,97,1)==6&&get(b,98,1)==63&&get(b,99,1)==0,7,"SATC","unsupported enum/layout");
zero(b,288,508);
o.N=get(b,32,4);
o.Ns=get(b,36,4);
o.M=get(b,40,4);
o.k0=get(b,80,8);
o.t0=I(get(b,48,8));
o.step=I(get(b,56,8));
o.start=I(get(b,64,8));
o.end=I(get(b,72,8));
o.frame=uint8_t(get(b,18,1));
o.stride=get(b,128,8);
need(o.N>0&&o.N<=1000000&&o.Ns==align(o.N,64)&&o.M>0&&o.step>=1000000&&o.stride==mul(o.Ns,48)&&get(b,120,8)==o.Ns*8&&o.end>o.start,7,"SATC","dimensions");
need(o.k0<=U(INT64_MAX)/U(o.step)&&o.t0==o.start+I(o.k0)*o.step,7,"SATC","global time");
U sc=get(b,20,4);
need(sc>=4&&get(b,24,8)==512&&add(512,mul(sc,32))<=b.size()-32,7,"SATC","directory");
U last=512+sc*32;
std::map<U,std::pair<U,U>>secs;
for(U j=0;j<sc;++j){
U d=512+j*32,t=get(b,d,4),off=get(b,d+8,8),sz=get(b,d+16,8),al=get(b,d+4,2);
need((al==6||al==12)&&off%(U(1)<<al)==0&&off>=last&&add(off,sz)<=b.size()-32,7,"SATC","section boundaries");
zero(b,d+6,d+8);
zero(b,d+28,d+32);
zero(b,last,off);
need(!secs.count(t),7,"SATC","duplicate section");
secs[t]={
off,sz}
;
if(t!=16)need(get(b,d+24,4)==crc(b.data()+off,size_t(sz)),7,"SATC","section CRC");
else need(get(b,d+24,4)==0&&al==12,7,"SATC","tensor CRC/alignment");
last=off+sz;
}
for(U t:{
1ULL,2ULL,3ULL,16ULL}
){
need(secs.count(t),7,"SATC","required section");
}
need(secs[1].second==o.Ns*12&&secs[2].second==o.Ns*24&&secs[3].second==align(o.M*8,64)+o.M*4&&secs[16].second==mul(o.M,o.stride),7,"SATC","section sizes");
o.tensor=secs[16].first;
U foot=b.size()-32;
need(last==foot&&gs(b,foot,4)=="SEND"&&get(b,foot+4,4)==crc(b.data()+512,size_t(sc*32))&&get(b,foot+8,8)==b.size()&&get(b,foot+16,8)==mul(o.M,o.stride),7,"SATC","footer");
zero(b,foot+24,foot+32);
o.s.resize(size_t(o.N));
U st=secs[1].first,na=secs[2].first;
for(U i=0;i<o.Ns;++i){
if(i>=o.N){
need(get(b,st+i*4,4)==UINT32_MAX,7,"SATC","gid padding");
zero(b,st+4*o.Ns+i*2,st+4*o.Ns+(i+1)*2);
zero(b,st+6*o.Ns+i*2,st+6*o.Ns+(i+1)*2);
zero(b,st+8*o.Ns+i*2,st+8*o.Ns+(i+1)*2);
zero(b,st+10*o.Ns+i,st+10*o.Ns+i+1);
zero(b,st+11*o.Ns+i,st+11*o.Ns+i+1);
zero(b,na+i*24,na+(i+1)*24);
continue;
}
auto&s=o.s[size_t(i)];
s.gid=uint32_t(get(b,st+i*4,4));
s.layer=uint16_t(get(b,st+4*o.Ns+i*2,2));
s.plane=uint16_t(get(b,st+6*o.Ns+i*2,2));
s.slot=uint16_t(get(b,st+8*o.Ns+i*2,2));
s.base=int(get(b,st+10*o.Ns+i,1));
s.name=gs(b,na+i*24,24);
utfcheck(s.name);
need(s.base<=2&&(i==0||s.gid>o.s[size_t(i-1)].gid),7,"SATC","catalog order/status");
}
if(secs.count(4)){
U a=secs[4].first,count=get(b,a,4);
need(secs[4].second==8+mul(count,24),7,"SATC","event size");
zero(b,a+4,a+8);
std::pair<U,I>prev{
0,INT64_MIN}
;
for(U j=0;j<count;++j){
U q=a+8+j*24,ix=get(b,q,4),stt=get(b,q+4,1);
I f=I(get(b,q+8,8)),t=I(get(b,q+16,8));
zero(b,q+5,q+8);
need(ix<o.N&&stt<=2&&f<t&&f>=o.t0&&t<=std::min(o.end,o.t0+I(o.M)*o.step),7,"SATC","event bounds");
auto&s=o.s[size_t(ix)];
need(stt!=U(s.base)&&std::make_pair(ix,f)>=prev&&(s.events.empty()||s.events.back().to<=f),7,"SATC","event ordering");
s.events.push_back({
int(stt),f,t}
);
prev={
ix,f}
;
}
}
need(bool(get(b,100,4)&2)==bool(secs.count(4)),7,"SATC","event flag");
U ti=secs[3].first;
zero(b,ti+o.M*8,ti+align(o.M*8,64));
for(U j=0;j<o.M;++j){
I t=o.t0+I(j)*o.step;
need(I(get(b,ti+j*8,8))==t&&t<o.end&&get(b,ti+align(o.M*8,64)+j*4,4)==crc(b.data()+o.tensor+j*o.stride,size_t(o.stride)),7,"SATC","time/slice CRC");
for(U i=0;i<o.Ns;++i){
bool allfinite=true,allnan=true;
for(int c=0;c<6;++c){
U off=o.tensor+j*o.stride+U(c)*o.Ns*8+i*8;
allfinite&=std::isfinite(gf(b,off));
allnan&=get(b,off,8)==0x7ff8000000000000ULL;
}
need(allfinite||allnan,7,"SATC","partial NaN/Inf/noncanonical NaN");
if(i>=o.N||status(o.s[size_t(i)],t)==2)need(allnan,7,"SATC","missing/padding not NaN");
}
}
return o;
}
std::string stem(U i){
std::ostringstream o;
o<<"pred_"<<std::setfill('0')<<std::setw(6)<<i;
return o.str();
}
void npy(const fs::path&p,std::vector<Sat>&s,U budget){
B b=load(p,budget);
need(b.size()>10&&b[0]==0x93&&gs(b,1,5)=="NUMPY"&&(b[6]==1||b[6]==2)&&b[7]==0,2,"npy","header");
U n=b[6]==1?get(b,8,2):get(b,8,4),o=b[6]==1?10:12;
need(add(o,n)<=b.size(),2,"npy","header length");
std::string h(reinterpret_cast<char*>(b.data()+o),size_t(n));
need(h.find("'<f8'")!=std::string::npos&&h.find("False")!=std::string::npos,2,"npy","only little-endian f8 C-order");
auto q=h.find("shape");
need(q!=std::string::npos,2,"npy","shape");
q=h.find('(',q);
auto e=h.find(')',q);
need(q!=std::string::npos&&e!=std::string::npos,2,"npy","shape");
auto dim=h.substr(q+1,e-q-1);
auto c=dim.find(',');
need(c!=std::string::npos&&uint(trim(dim.substr(0,c)))==s.size()&&trim(dim.substr(c+1))=="6",2,"npy","shape must (N,6)");
o+=n;
need(b.size()==add(o,mul(s.size(),48)),2,"npy","data size");
for(size_t i=0;i<s.size();++i){
for(int c=0;c<6;++c)s[i].y.q[c]=gf(b,o+i*48+U(c)*8);
need(valid(s[i].y),2,"npy","invalid initial state");
}
}
J failures(const Config&c){
J a=J::arr();
for(auto&s:c.s)if(s.failed){
J j=J::obj();
j["gid"]=U(s.gid);
j["time_ns"]=s.failuretime;
j["reason"]=s.reason;
a.a.push_back(j);
}
return a;
}
fs::path predict(const Options&o,fs::path out={
}
){
Run r=runtime(o);
need(o.m.at("frame")=="eci"||o.m.at("frame")=="ecef",1,"frame","eci/ecef only");
Settings set=settings(fs::u8path(o.m.at("settings")));
Config cfg=constellation(fs::u8path(o.m.at("config")),r.start,r.end,o.m.at("exclude-missing")=="true",o.m.at("exclude-damaged")=="true",&set.phys);
if(o.m.at("initial-npy")!="none")npy(fs::u8path(o.m.at("initial-npy")),cfg.s,r.budget);
Environment env{
r.start,r.ens,set,-1,{
}
,{
}
,0}
;
env.node(0);
env.node(1);
U lastnode=std::max<U>(1,(mul(r.M-1,U(r.step))+U(r.ens)-1)/U(r.ens));
env.node(I(lastnode));
if(!set.eopT.empty()||!set.ephT.empty()||!set.denT.empty()){
U maxnode=lastnode+1;
I hi=r.start+I(maxnode)*r.ens;
if(!set.eopT.empty())need(set.eopT.front()<=r.start-500000000LL&&set.eopT.back()>=hi+500000000LL,2,"eop_csv","table does not cover run window +-0.5s");
if(!set.ephT.empty())need(set.ephT.front()<=r.start&&set.ephT.back()>=hi,2,"ephemeris_csv","table does not cover run window");
if(!set.denT.empty())need(set.denT.front()<=r.start&&set.denT.back()>=hi,2,"density_csv","table does not cover run window");
}
if(out.empty())out=newrun(o.m.at("output"));
else fs::create_directories(out);
atomic(out/"effective_config.json",effective(o).dump());
B init;
for(auto&s:cfg.s)for(auto v:s.y.q)appendf(init,v);
J identity=J::obj();
identity["implementation"]="satpred-mvp-0.1";
identity["effective"]=effective(o);
for(auto k:{
"output","threads","batch","memory-budget"}
)identity["effective"].o.erase(k);
identity["config_sha256"]=hex(cfg.config);
identity["geometry_sha256"]=hex(cfg.geometry);
identity["initial_state_sha256"]=hex(sha(init));
identity["settings_sha256"]=hex(sha(set.raw));
if(!set.ext.empty()){
J ex=J::arr();
for(auto&e:set.ext){J q=J::obj();q["role"]=e.role;q["sha256"]=e.sha;q["size"]=e.size;ex.a.push_back(q);}
identity["external_data"]=ex;
}
B id=sha(std::string("satpred-mvp-run-v1\0",19)+identity.dump());
J products=J::arr();
for(U fi=0;fi<r.files;++fi){
B b=writeorbit(cfg,r,fi,env,id);
finalizeorbit(b,env,o.m.at("frame")=="ecef");
fs::path p=out/(stem(fi)+".sbin");
atomic(p,b);
auto verified=readorbit(p,r.budget);
J q=J::obj();
q["file"]=pathtext(p.filename());
q["M"]=verified.M;
q["M_full"]=r.full;
q["t0_ns"]=verified.t0;
q["global_index0"]=verified.k0;
q["size"]=U(b.size());
q["sha256"]=hex(sha(b));
products.a.push_back(q);
}
need(std::any_of(cfg.s.begin(),cfg.s.end(),[](auto&s){
return !s.failed;}
),4,"propagation","all Catalog members failed");
J m=J::obj();
m["format"]="SATPRED-B-1";
auto ff=failures(cfg);
m["status"]=ff.a.empty()?"complete":"complete_with_failures";
m["software"]=J::obj();
m["software"]["version"]="0.1.0-mvp";
m["software"]["actual_backend"]="cpu";
m["software"]["threads"]=r.threads;
m["run"]=J::obj();
auto&run=m["run"];
run["run_id"]=hex(id);
run["start_ns"]=r.start;
run["end_ns"]=r.end;
run["step_ns"]=r.step;
run["package_ns"]=r.package;
run["frame"]=o.m.at("frame");
run["frame_method"]="approximate-j2000-gmst-precession";
run["max_h_ns"]=r.maxh;
run["env_step_ns"]=r.ens;
run["env_orthogonality_max"]=env.orthmax;
m["inputs"]=J::obj();
m["inputs"]["config_sha256"]=hex(cfg.config);
m["inputs"]["geometry_sha256"]=hex(cfg.geometry);
m["inputs"]["initial_state_sha256"]=hex(sha(init));
m["inputs"]["settings_sha256"]=hex(sha(set.raw));
{
J ex=J::arr();
std::vector<Settings::Ext> es=set.ext;
std::sort(es.begin(),es.end(),[](const Settings::Ext&a,const Settings::Ext&b){return a.role<b.role||(a.role==b.role&&a.sha<b.sha);});
for(auto&e:es){J q=J::obj();q["role"]=e.role;q["sha256"]=e.sha;q["size"]=e.size;ex.a.push_back(q);}
m["inputs"]["external_data"]=ex;
}
m["inputs"]["initial_mode"]=o.m.at("initial-npy")=="none"?"analytic":"npy";
m["catalog"]=J::obj();
m["catalog"]["N"]=U(cfg.s.size());
m["catalog"]["Ns"]=align(cfg.s.size(),64);
J a=J::arr();
for(size_t i=0;i<cfg.s.size();++i){
auto&s=cfg.s[i];
J j=J::obj();
j["index"]=U(i);
j["gid"]=U(s.gid);
j["name"]=s.name;
j["layer"]=int(s.layer);
j["plane"]=int(s.plane);
j["slot"]=int(s.slot);
j["status_base"]=s.base;
a.a.push_back(j);
}
m["catalog"]["members"]=a;
{
J ps=J::arr();
std::set<uint32_t> nu(cfg.physNotUsed.begin(),cfg.physNotUsed.end());
for(auto&kv:set.phys){
J q=J::obj();
q["gid"]=U(kv.first);
q["source"]=kv.second.source?kv.second.source_v:"placeholder";
q["reference"]=kv.second.reference?kv.second.reference_v:"";
q["not_used"]=nu.count(kv.first)!=0;
ps.a.push_back(q);
}
m["phys_sources"]=ps;
}
m["products"]=products;
m["failures"]=ff;
J warnings=J::arr();
warnings.a.emplace_back("MVP: not full v3.4 acceptance; run_id is MVP identity, not section 11 canonical identity");
warnings.a.emplace_back("crash_durability_certified=false");
m["warnings"]=warnings;
atomic(out/"manifest.json",m.dump());
return out;
}
struct Policy{
double search=5e6,access=3e6,elev=25*pi/180,ref=1e6,alpha=2;
U k=4;
uint32_t proc=0;
bool wrap=false,accessfirst=false,exd=false;
std::string topology="mesh";
}
;
Policy policy(const Options&o){
Policy p;
p.search=real(o.m.at("search-range-km"))*1000;
p.access=real(o.m.at("access-range-km"))*1000;
p.ref=real(o.m.at("reference-distance-km"))*1000;
p.elev=real(o.m.at("minimum-elevation"))*pi/180;
p.alpha=real(o.m.at("distance-exponent"));
p.k=uint(o.m.at("knn-k"));
U pr=uint(o.m.at("processing-us"));
need(std::isfinite(p.search)&&p.search>0&&std::isfinite(p.access)&&p.access>0&&std::isfinite(p.ref)&&p.ref>0&&p.elev>=0&&p.elev<=pi/2&&p.alpha>=0&&p.k>0&&p.k<=UINT32_MAX&&pr<=UINT32_MAX,1,"policy","network parameter range");
p.proc=uint32_t(pr);
p.wrap=o.m.at("wrap-planes")=="true";
p.accessfirst=o.m.at("access-first")=="true";
p.exd=o.m.at("exclude-damaged")=="true";
p.topology=o.m.at("topology");
need(p.topology=="mesh"||p.topology=="knn",1,"topology","unsupported");
return p;
}
U bandwidth(U cap,double g){
if(g>=1)return cap;
if(g<=0)return 0;
int e;
double f=std::frexp(g,&e);
U m=U(std::ldexp(f,53));
unsigned shift=unsigned(53-e);
// portable exact 64x53 -> 128, four 32-bit limbs
U a0=uint32_t(cap),a1=cap>>32,b0=uint32_t(m),b1=m>>32;
U p0=a0*b0,p1=a0*b1,p2=a1*b0,p3=a1*b1;
U middle=(p0>>32)+uint32_t(p1)+uint32_t(p2);
U lo=(middle<<32)|uint32_t(p0),hi=p3+(p1>>32)+(p2>>32)+(middle>>32);
if(shift>=128)return 0;
if(shift>=64)return hi>>(shift-64);
return (lo>>shift)|(hi<<(64-shift));
}
bool visible(V a,V b){
a={
a.x/RE,a.y/RE,a.z/RB}
;
b={
b.x/RE,b.y/RE,b.z/RB}
;
V v=b-a;
double d=dot(v,v);
if(d<=0||!std::isfinite(d))return false;
double u=std::clamp(-dot(a,v)/d,0.0,1.0);
return dot(a+v*u,a+v*u)>=1-1e-12;
}
struct Edge{
uint32_t src,dst,delay,kind;
U bw;
double d;
}
;
B edgesbytes(const std::vector<Edge>&e){
B b(e.size()*32);
for(size_t i=0;i<e.size();++i){
auto&a=e[i];
U o=i*32;
put(b,o,a.src,4);
put(b,o+4,a.dst,4);
put(b,o+8,a.delay,4);
put(b,o+12,a.bw,8);
pf(b,o+20,a.d);
put(b,o+28,a.kind,4);
}
return b;
}
using Cell=std::tuple<I,I,I>;
struct Spatial{
double h;
const std::vector<V>&r;
std::map<Cell,std::vector<uint32_t>>grid;
Cell cell(V v)const{
auto cv=[&](double d){
double x=std::floor(d/h);
need(std::isfinite(x)&&x>-9e18&&x<9e18,7,"spatial","cell overflow");
return I(x);
}
;
return{
cv(v.x),cv(v.y),cv(v.z)}
;
}
Spatial(double h,const std::vector<V>&r,const std::vector<bool>&valid):h(h),r(r){
for(size_t i=0;i<r.size();++i)if(valid[i])grid[cell(r[i])].push_back(uint32_t(i));
}
std::vector<uint32_t>query(V v,double radius)const{
std::vector<uint32_t>out;
auto [x,y,z]=cell(v);
for(I dx=-1;dx<=1;++dx)for(I dy=-1;dy<=1;++dy)for(I dz=-1;dz<=1;++dz){
auto it=grid.find({
x+dx,y+dy,z+dz}
);
if(it==grid.end())continue;
for(auto i:it->second)if(norm(r[i]-v)<=radius)out.push_back(i);
}
return out;
}
}
;
std::vector<Edge>connect(const Config&c,const Orbit&o,U j,const std::vector<Node>&ground,const Policy&p,U budget){
size_t N=c.s.size();
I time=o.t0+I(j)*o.step;
std::vector<V>r(N);
std::vector<bool>valids(N);
for(size_t i=0;i<N;++i){
State y=o.at(j,i);
r[i]=y.r();
valids[i]=status(o.s[i],time)!=2&&std::all_of(y.q.begin(),y.q.end(),[](double d){
return std::isfinite(d);}
)&&(!p.exd||status(o.s[i],time)!=1);
}
Spatial grid(std::max(p.search,p.access),r,valids);
std::set<std::pair<uint32_t,uint32_t>>pairs;
auto insert=[&](U a,U b){
if(a==b)return;
if(a>b)std::swap(a,b);
need(mul(pairs.size()+1,160)<budget/4,2,"network","candidate memory budget exceeded");
pairs.emplace(uint32_t(a),uint32_t(b));
}
;
if(p.topology=="mesh"){
std::map<std::tuple<int,int,int>,uint32_t>lookup;
for(size_t i=0;i<N;++i)lookup[{
c.s[i].layer,c.s[i].plane,c.s[i].slot}
]=uint32_t(i);
for(size_t i=0;i<N;++i)if(valids[i]){
auto&s=c.s[i];
auto take=[&](int pl,int sl){
auto it=lookup.find({
s.layer,pl,sl}
);
if(it!=lookup.end()&&valids[it->second])insert(i,it->second);
}
;
take(s.plane,(s.slot+1)%int(s.S));
take(s.plane,(int(s.slot)+int(s.S)-1)%int(s.S));
if(s.plane>0)take(s.plane-1,s.slot);
else if(p.wrap)take(int(s.P)-1,s.slot);
if(U(s.plane)+1<s.P)take(s.plane+1,s.slot);
else if(p.wrap)take(0,s.slot);
}
}
else for(size_t i=0;i<N;++i)if(valids[i]){
auto ids=grid.query(r[i],p.search);
std::vector<std::pair<double,uint32_t>>ds;
for(auto k:ids)if(k!=i)ds.push_back({
norm(r[i]-r[k]),k}
);
std::sort(ds.begin(),ds.end());
for(size_t k=0;k<std::min<size_t>(size_t(p.k),ds.size());++k)insert(i,ds[k].second);
}
for(size_t g=0;g<ground.size();++g)for(auto i:grid.query(ground[g].xyz,p.access))insert(i,N+g);
struct Link{
uint32_t a,b,kind,delay;
double d;
U ab,ba;
}
;
std::vector<Link>ls;
std::vector<Radio>rad;
for(auto&s:c.s)rad.push_back(s.radio);
for(auto&g:ground)rad.push_back(g.radio);
for(auto [a,b]:pairs){
V A=r[a],B=b<N?r[b]:ground[b-N].xyz;
double d=norm(B-A);
int kind=b<N?0:1;
if(!std::isfinite(d)||d<=0||d>(kind?p.access:p.search)||!visible(A,B))continue;
if(kind&&dot(A-B,ground[b-N].up)/d+1e-12<std::sin(p.elev))continue;
double gain=std::clamp(std::pow(p.ref/std::max(p.ref,d),p.alpha),0.0,1.0);
U ab=bandwidth(std::min(rad[a].up,rad[b].down),gain),ba=bandwidth(std::min(rad[b].up,rad[a].down),gain);
if(!ab&&!ba)continue;
double delay=std::ceil(d/299792458.0*1e6+p.proc);
need(std::isfinite(delay)&&delay>=0&&delay<=UINT32_MAX,1,"delay","uint32 overflow");
ls.push_back({
a,b,uint32_t(kind),uint32_t(delay),d,ab,ba}
);
}
std::sort(ls.begin(),ls.end(),[&](auto&a,auto&b){
int ak=p.accessfirst?1-int(a.kind):int(a.kind),bk=p.accessfirst?1-int(b.kind):int(b.kind);return std::tie(ak,a.d,a.a,a.b)<std::tie(bk,b.d,b.a,b.b);}
);
std::vector<U>used(rad.size());
std::vector<Edge>e;
for(auto&l:ls){
// Access links (kind 1) are candidates: every visible satellite is kept and no antenna is consumed on
// either side. The terminal's antenna count does not limit its candidate satellites; the actual
// satellite selection (handover policy) is performed downstream in the network simulator.
// Inter-satellite links still share the satellite antenna budget.
if(l.kind==0){
if(used[l.a]>=rad[l.a].ant||used[l.b]>=rad[l.b].ant)continue;
++used[l.a];
++used[l.b];
}
if(l.ab)e.push_back({
l.a,l.b,l.delay,l.kind,l.ab,l.d}
);
if(l.ba)e.push_back({
l.b,l.a,l.delay,l.kind,l.ba,l.d}
);
}
std::sort(e.begin(),e.end(),[](auto&a,auto&b){
return std::tie(a.src,a.dst)<std::tie(b.src,b.dst);}
);
return e;
}
J models(const Policy&p){
J m=J::obj();
for(auto kv:std::vector<std::pair<std::string,std::string>>{
{
"topology",p.topology}
,{
"resource","duplex-shared-antennas-isl-only"}
,{
"access","all-visible-candidates-selection-in-simulator"}
,{
"bandwidth","distance-power"}
,{
"delay","propagation-plus-processing"}
,{
"occlusion","ellipsoid-segment"}
}
){
J q=J::obj();
q["name"]=kv.second;
q["version"]=1;
m[kv.first]=q;
}
return m;
}
J vjson(V v){
J a=J::arr();
a.a={
J(v.x),J(v.y),J(v.z)}
;
return a;
}
J nodemeta(const Config&c,const std::vector<Node>&g){
J a=J::arr();
for(size_t i=0;i<c.s.size();++i){
auto&s=c.s[i];
J j=J::obj();
j["id"]=U(i);
j["kind"]=0;
j["name"]=s.explicitname.empty()?"sat"+std::to_string(U(s.gid)+1):s.explicitname;
j["uplink_bps"]=s.radio.up;
j["downlink_bps"]=s.radio.down;
j["antennas"]=U(s.radio.ant);
j["gid"]=U(s.gid);
j["layer"]=int(s.layer);
j["plane"]=int(s.plane);
j["slot"]=int(s.slot);
j["P"]=U(s.P);
j["S"]=U(s.S);
a.a.push_back(j);
}
for(auto&n:g){
J j=J::obj();
j["id"]=U(n.id);
j["kind"]=n.kind;
j["name"]=n.name;
j["uplink_bps"]=n.radio.up;
j["downlink_bps"]=n.radio.down;
j["antennas"]=U(n.radio.ant);
j["xyz"]=vjson(n.xyz);
j["up"]=vjson(n.up);
a.a.push_back(j);
}
return a;
}
B graphwrite(const Config&c,const Orbit&o,const std::vector<Node>&g,const Policy&p,J metadata,const B&source,U budget){
auto ms=metadata.dump();
U ti=align(256+ms.size(),64),po=align(ti+o.M*32,4096);
B b(static_cast<size_t>(po), uint8_t(0));
ps(b,0,"SATNET1",8);
put(b,8,1,2);
put(b,10,1,2);
put(b,12,256,4);
put(b,20,c.s.size()+g.size(),4);
put(b,24,o.M,4);
put(b,28,32,4);
put(b,32,256,8);
put(b,40,ms.size(),8);
put(b,48,ti,8);
put(b,56,po,8);
put(b,64,U(o.t0),8);
put(b,72,U(o.step),8);
put(b,80,o.k0,8);
std::copy(source.begin(),source.end(),b.begin()+104);
ps(b,256,ms,ms.size());
U total=0;
for(U j=0;j<o.M;++j){
safe();
auto edges=connect(c,o,j,g,p,budget);
B e=edgesbytes(edges);
need(add(b.size(),e.size())<budget/3,2,"graph","MVP package buffer exceeds budget");
U d=ti+j*32;
put(b,d,U(o.t0+I(j)*o.step),8);
put(b,d+8,b.size(),8);
put(b,d+16,edges.size(),8);
put(b,d+24,crc(e),4);
total+=edges.size();
b.insert(b.end(),e.begin(),e.end());
}
U footer=b.size();
b.resize(b.size()+32);
ps(b,footer,"NETEND1",8);
put(b,footer+8,crc(b.data()+ti,size_t(o.M*32)),4);
put(b,footer+12,crc(reinterpret_cast<const uint8_t*>(ms.data()),ms.size()),4);
put(b,footer+16,b.size(),8);
put(b,footer+24,total*32,8);
put(b,88,total,8);
put(b,96,b.size(),8);
put(b,252,crc(b.data(),252),4);
return b;
}
struct Graph{
B b;
J meta;
U M,N,ti,po,total,k0;
I t0,step;
std::vector<Edge>at(U j)const{
U d=ti+j*32,off=get(b,d+8,8),n=get(b,d+16,8);
std::vector<Edge>e;
for(U k=0;k<n;++k){
U a=off+k*32;
e.push_back({
uint32_t(get(b,a,4)),uint32_t(get(b,a+4,4)),uint32_t(get(b,a+8,4)),uint32_t(get(b,a+28,4)),get(b,a+12,8),gf(b,a+20)}
);
}
return e;
}
}
;
Graph graphread(const fs::path&p,U budget,const fs::path&source={
}
){
Graph g;
g.b=load(p,budget);
auto&b=g.b;
need(b.size()>=288&&gs(b,0,8)=="SATNET1"&&get(b,8,2)==1&&get(b,10,2)<=1&&get(b,12,4)==256&&get(b,28,4)==32&&get(b,252,4)==crc(b.data(),252),7,"SATNET","header");
need(get(b,16,4)<=1,7,"SATNET","flags");
zero(b,136,252);
g.N=get(b,20,4);
g.M=get(b,24,4);
U mo=get(b,32,8),ms=get(b,40,8);
g.ti=get(b,48,8);
g.po=get(b,56,8);
g.t0=I(get(b,64,8));
g.step=I(get(b,72,8));
g.k0=get(b,80,8);
g.total=get(b,88,8);
need(g.N>0&&g.M>0&&g.step>=1000000&&get(b,96,8)==b.size()&&mo==256&&add(mo,ms)<=g.ti&&g.ti%64==0&&add(g.ti,mul(g.M,32))<=g.po&&g.po%4096==0&&add(g.po,mul(g.total,32))==b.size()-32,7,"SATNET","boundaries");
zero(b,mo+ms,g.ti);
zero(b,g.ti+g.M*32,g.po);
std::string text(reinterpret_cast<const char*>(b.data()+mo),size_t(ms));
g.meta=json(text);
auto&nodes=g.meta.at("nodes");
need(nodes.type==J::Arr&&nodes.a.size()==g.N,7,"SATNET","nodes");
for(U i=0;i<g.N;++i){
auto&n=nodes.a[size_t(i)];
need(n.at("id").u()==i&&n.at("kind").u()<=2&&n.at("antennas").u()<=UINT32_MAX,7,"SATNET","node metadata");
}
U end=g.po,total=0;
for(U j=0;j<g.M;++j){
U d=g.ti+j*32,off=get(b,d+8,8),n=get(b,d+16,8),sz=mul(n,32);
need(I(get(b,d,8))==g.t0+I(j)*g.step&&off==end&&add(off,sz)<=b.size()-32&&get(b,d+24,4)==crc(b.data()+off,size_t(sz)),7,"SATNET","slice index/CRC");
zero(b,d+28,d+32);
auto es=g.at(j);
std::set<std::pair<U,U>>duplex;
std::pair<U,U>prev{
0,0}
;
bool first=true;
std::vector<U>used(size_t(g.N),0);
for(auto&e:es){
auto pair=std::make_pair(U(e.src),U(e.dst));
need(e.src<g.N&&e.dst<g.N&&e.src!=e.dst&&e.bw>0&&std::isfinite(e.d)&&e.d>0&&e.kind<=1&&(first||pair>prev),7,"SATNET","edge invalid/order");
bool sat1=nodes.a[e.src].at("kind").u()==0,sat2=nodes.a[e.dst].at("kind").u()==0;
need((e.kind==0&&sat1&&sat2)||(e.kind==1&&sat1!=sat2),7,"SATNET","kind");
first=false;
prev=pair;
U a=std::min(e.src,e.dst),bb=std::max(e.src,e.dst);
if(duplex.emplace(a,bb).second&&e.kind==0){
++used[size_t(a)];
++used[size_t(bb)];
}
}
for(U i=0;i<g.N;++i){
need(used[size_t(i)]<=nodes.a[size_t(i)].at("antennas").u(),7,"SATNET","antenna overuse (inter-satellite links)");
}
end=off+sz;
total=add(total,n);
}
U f=b.size()-32;
need(total==g.total&&end==f&&gs(b,f,8)=="NETEND1"&&get(b,f+8,4)==crc(b.data()+g.ti,size_t(g.M*32))&&get(b,f+12,4)==crc(b.data()+mo,size_t(ms))&&get(b,f+16,8)==b.size()&&get(b,f+24,8)==g.total*32,7,"SATNET","footer");
B headerSHA(b.begin()+104,b.begin()+136);
if(get(b,16,4)==1)zero(b,104,136);
else need(g.meta.at("source_hash_available").flag()&&g.meta.at("source_orbit_sha256").str()==hex(headerSHA),7,"SATNET","metadata SHA binding");
if(!source.empty()){
Orbit o=readorbit(source,budget);
need(o.frame==0&&o.M==g.M&&o.t0==g.t0&&o.step==g.step&&o.k0==g.k0&&filesha(source)==headerSHA&&o.N<=g.N,7,"source","SHA/frame/time");
for(U i=0;i<o.N;++i)need(nodes.a[size_t(i)].at("kind").u()==0&&nodes.a[size_t(i)].at("gid").u()==o.s[size_t(i)].gid,7,"source","Catalog binding");
}
return g;
}
std::string fixed(double x,int n){
std::ostringstream s;
s.imbue(std::locale::classic());
s<<std::fixed<<std::setprecision(n)<<x;
return s.str();
}
std::string position(V v){
double p=std::hypot(v.x,v.y),e2=1-RB*RB/(RE*RE),lat,lon,h;
if(p<1e-9){
need(std::abs(v.z)>0,7,"geodetic","origin");
lat=v.z>0?pi/2:-pi/2;
lon=0;
h=std::abs(v.z)-RB;
}
else{
lon=std::atan2(v.y,v.x);
lat=std::atan2(v.z,p*(1-e2));
bool ok=false;
for(int i=0;i<20;++i){
double N=RE/std::sqrt(1-e2*std::sin(lat)*std::sin(lat)),next=std::atan2(v.z+e2*N*std::sin(lat),p);
if(std::abs(next-lat)<=1e-13){
lat=next;
ok=true;
break;
}
lat=next;
}
need(ok,5,"geodetic","iteration did not converge");
h=p*std::cos(lat)+v.z*std::sin(lat)-RE*std::sqrt(1-e2*std::sin(lat)*std::sin(lat));
}
return fixed(lat*180/pi,6)+","+fixed(lon*180/pi,6)+","+fixed(h/1000,2);
}
std::string unittext(U v,U divisor,int digits,const char*suffix){
std::ostringstream s;
s<<v/divisor<<'.'<<std::setfill('0')<<std::setw(digits)<<v%divisor<<suffix;
return s.str();
}
void links(const fs::path&p,const Graph&g,const Orbit&o,const B&gsha){
auto part=p;
part+=".part";
try{
std::ofstream f(part,std::ios::binary);
need(bool(f),3,"links","open");
f<<"{\"format\":\"satnet-links-1\",\"run_id\":"<<g.meta.at("run_id").dump()<<",\"source_graph_sha256\":"<<quote(hex(gsha))<<",\"source_orbit_sha256\":"<<g.meta.at("source_orbit_sha256").dump()<<",\"models\":"<<g.meta.at("models").dump();
if (g.meta.o.count("case_name")) f << ",\"case_name\":" << g.meta.at("case_name").dump();
if (g.meta.o.count("rf_parameters")) f << ",\"rf_parameters\":" << g.meta.at("rf_parameters").dump();
f << ",\"slices\":[";
auto&nodes=g.meta.at("nodes").a;
auto pos=[&](U j,U i){
if(i<o.N)return o.at(j,i).r();
auto&a=nodes[size_t(i)].at("xyz").a;
return V{
a[0].real(),a[1].real(),a[2].real()}
;
}
;
for(U j=0;j<g.M;++j){
safe();
I t=g.t0+I(j)*g.step;
if(j)f<<',';
f<<"{\"timestamp_ns\":"<<t<<",\"valid_from_ns\":"<<t<<",\"valid_to_ns\":"<<std::min(t+g.step,o.end)<<",\"links\":[";
auto es=g.at(j);
for(size_t k=0;k<es.size();++k){
if(k)f<<',';
auto&e=es[k];
J q=J::obj();
q["endpoint1"]=nodes[e.src].at("name");
q["endpoint2"]=nodes[e.dst].at("name");
q["rate"]=unittext(e.bw,1000000,6,"mbit");
q["delay"]=unittext(e.delay,1000,3,"ms");
q["loss"]="0.0";
q["pos1"]=position(pos(j,e.src));
q["pos2"]=position(pos(j,e.dst));
q["src"]=U(e.src);
q["dst"]=U(e.dst);
q["bandwidth_bps"]=e.bw;
q["delay_us"]=U(e.delay);
q["distance_m"]=e.d;
q["kind"]=U(e.kind);
f<<q.dump();
}
f<<"]}";
}
f<<"]}";
f.flush();
need(bool(f),3,"links","write");
f.close();
safe();
fs::rename(part,p);
}
catch(...){
std::error_code ec;
fs::remove(part,ec);
throw;
}
}
// Best-effort convenience index for satread/netread default input selection.
// Run products are already complete and verified when this is called, so a
// failure to write the per-user state file must not fail the run (see
// RELEASE_NOTES.md: default-flow exit 3 after SATC_INTEGRITY_OK).
bool updatesuccess(const fs::path&out,const fs::path&orbit,const fs::path&graph){
J j=J::obj();
j["run_dir"]=pathtext(fs::absolute(out));
j["orbit"]=pathtext(fs::absolute(orbit));
j["graph"]=pathtext(fs::absolute(graph));
try{
atomic(home()/"last_success.json",j.dump());
return true;
}
catch(const Err&e){
if(e.where=="run")throw; // cancellation is not a state-file failure
std::cerr<<"{\"warning\":\"last_success_not_recorded\",\"where\":"<<quote(e.where)<<",\"message\":"<<quote(e.what())<<"}\n";
}
catch(const std::exception&e){
std::cerr<<"{\"warning\":\"last_success_not_recorded\",\"where\":\"filesystem\",\"message\":"<<quote(e.what())<<"}\n";
}
return false;
}
fs::path network(const Options&o){
Run r=runtime(o);
Policy pol=policy(o);
auto backend=o.m.at("backend");
need(backend=="cpu"||backend=="auto"||backend=="cuda",1,"backend","unknown");
if(backend=="cuda")fail(6,"backend","CUDA not implemented/available");
Config cfg=constellation(fs::u8path(o.m.at("satellites")),r.start,r.end,false,false);
auto ground=grounds(fs::u8path(o.m.at("nodes")),cfg.s.size());
fs::path out=newrun(o.m.at("output")),orbitdir=out/"orbits";
fs::create_directories(orbitdir);
fs::create_directories(out/"graphs");
fs::create_directories(out/"exports");
atomic(out/"effective_config.json",effective(o).dump());
if(o.m.at("orbit-dir")=="none"){
std::vector<std::string>a={
"--config",o.m.at("satellites"),"--frame","ecef","--start",o.m.at("start"),"--duration",o.m.at("duration"),"--step",o.m.at("step"),"--package",o.m.at("package"),"--output",pathtext(orbitdir),"--threads",o.m.at("threads"),"--memory-budget",o.m.at("memory-budget")}
;
child(fs::u8path(o.m.at("orbit-program")),a);
}
else{
fs::path src=fs::u8path(o.m.at("orbit-dir"));
if(fs::exists(src/"samples"))src/= "samples";
need(fs::is_directory(src),3,"snapshot","source directory absent");
std::set<std::string>names;
for(auto&e:fs::directory_iterator(src)){
if(e.path().extension()!=".sbin")continue;
safe();
auto p=e.path();
need(names.insert(pathtext(p.filename())).second,7,"snapshot","duplicate stem");
U size=fs::file_size(p);
auto mtime=fs::last_write_time(p);
fs::copy_file(p,orbitdir/p.filename());
need(size==fs::file_size(p)&&mtime==fs::last_write_time(p),7,"snapshot","source changed during copy");
}
if(fs::exists(src/"manifest.json"))fs::copy_file(src/"manifest.json",orbitdir/"manifest.json");
}
std::vector<std::pair<U,fs::path>>paths;
for(auto&e:fs::directory_iterator(orbitdir))if(e.path().extension()==".sbin"){
auto b=load(e.path(),r.budget);
need(b.size()>=512,7,"source","short header");
paths.push_back({
get(b,80,8),e.path()}
);
}
std::sort(paths.begin(),paths.end());
need(!paths.empty(),7,"source","no orbit packages");
J sources=J::arr();
U expected=0;
B firstidentity;
U packageindex=0;
for(auto&[k,p]:paths){
Orbit oo=readorbit(p,r.budget);
need(k==expected&&oo.N==cfg.s.size()&&oo.frame==0&&oo.start==r.start&&oo.end==r.end&&oo.step==r.step&&I(get(oo.bytes,136,8))==r.package&&get(oo.bytes,152,8)==get(cfg.config,0,8),7,"source","gap/identity/grid mismatch");
for(size_t i=0;i<cfg.s.size();++i)need(oo.s[i].gid==cfg.s[i].gid&&oo.s[i].layer==cfg.s[i].layer&&oo.s[i].plane==cfg.s[i].plane&&oo.s[i].slot==cfg.s[i].slot,7,"source","Catalog mismatch");
need(get(oo.bytes,40,4)==std::min(r.full,r.M-expected)&&get(oo.bytes,44,4)==r.full&&get(oo.bytes,88,4)==packageindex&&get(oo.bytes,92,4)==paths.size()&&get(oo.bytes,144,8)==get(cfg.geometry,0,8),7,"source","package/geometry mismatch");
B currentidentity(oo.bytes.begin()+168,oo.bytes.begin()+200);
if(packageindex==0)firstidentity=currentidentity;
else need(currentidentity==firstidentity,7,"source","cross-package run identity mismatch");
++packageindex;
expected+=oo.M;
J s=J::obj();
s["global_index0"]=k;
s["sha256"]=hex(filesha(p));
sources.a.push_back(s);
}
need(expected==r.M,7,"source","total sample count");
if(fs::exists(orbitdir/"manifest.json")){
J m=json(readtext(orbitdir/"manifest.json"));
need(m.at("inputs").at("config_sha256").str()==hex(cfg.config),7,"source","full XML SHA mismatch");
}
J nodes=nodemeta(cfg,ground);
atomic(out/"nodes.json",nodes.dump());
J ident=J::obj();
ident["implementation"]="satnet-mvp-0.1";
ident["sources"]=sources;
ident["nodes"]=nodes;
ident["effective"]=effective(o);
for(auto k:{
"output","threads","memory-budget","backend","orbit-program","orbit-dir"}
)ident["effective"].o.erase(k);
B id=sha(ident.dump());
J products=J::arr();
U edgecount=0;
for(size_t z=0;z<paths.size();++z){
auto&src=paths[z].second;
Orbit orb=readorbit(src,r.budget);
B source=filesha(src);
J meta=J::obj();
meta["format"]="satnet-metadata-1.1";
meta["software_version"]="0.1.0-mvp";
meta["actual_backend"]="cpu";
meta["run_id"]=hex(id);
meta["satellite_xml_sha256"]=hex(cfg.config);
meta["ground_xml_sha256"]=hex(filesha(fs::u8path(o.m.at("nodes"))));
if (oneweb_small::matches(meta.at("satellite_xml_sha256").str(), meta.at("ground_xml_sha256").str())) {
meta["case_name"] = oneweb_small::name;
meta["rf_parameters"] = oneweb_small::rf_metadata<J>();
}
meta["source_hash_available"]=true;
meta["source_orbit_sha256"]=hex(source);
meta["source_accuracy_class"]=2;
meta["source_frame_method"]="approximate-j2000-gmst-precession";
meta["nodes"]=nodes;
meta["effective_policy"]=effective(o);
meta["models"]=models(pol);
J ass=J::obj();
for(auto k:{
"fixed_ground","per_link_capacity","not_rf_budget","no_routing"}
)ass[k]=true;
ass["processing_included"]=pol.proc>0;
meta["assumptions"]=ass;
auto graph=out/"graphs"/(pathtext(src.stem())+".ngb"),link=out/"exports"/(pathtext(src.stem())+".links.json");
B b=graphwrite(cfg,orb,ground,pol,meta,source,r.budget);
atomic(graph,b);
Graph g=graphread(graph,r.budget,src);
links(link,g,orb,sha(b));
need(json(readtext(link,r.budget)).at("slices").a.size()==orb.M,7,"links","slice count");
J q=J::obj();
q["source_file"]=pathtext(fs::relative(src,out));
q["source_sha256"]=hex(source);
q["graph_file"]=pathtext(fs::relative(graph,out));
q["graph_sha256"]=hex(sha(b));
q["links_file"]=pathtext(fs::relative(link,out));
q["links_sha256"]=hex(filesha(link));
q["M"]=orb.M;
q["t0_ns"]=orb.t0;
q["global_index0"]=orb.k0;
q["directed_edges"]=g.total;
edgecount+=g.total;
products.a.push_back(q);
}
J m=J::obj();
m["format"]="SATNET-1.1";
m["status"]="complete";
m["software"]=J::obj();
m["software"]["version"]="0.1.0-mvp";
m["software"]["actual_backend"]="cpu";
m["software"]["threads"]=r.threads;
m["run"]=J::obj();
m["run"]["run_id"]=hex(id);
m["run"]["start_ns"]=r.start;
m["run"]["end_ns"]=r.end;
m["run"]["step_ns"]=r.step;
m["run"]["policy"]=effective(o);
m["run"]["models"]=models(pol);
m["inputs"]=J::obj();
m["inputs"]["satellite_xml_sha256"]=hex(cfg.config);
m["inputs"]["ground_xml_sha256"]=hex(filesha(fs::u8path(o.m.at("nodes"))));
if (oneweb_small::matches(m["inputs"].at("satellite_xml_sha256").str(), m["inputs"].at("ground_xml_sha256").str())) {
m["case_name"] = oneweb_small::name;
m["rf_parameters"] = oneweb_small::rf_metadata<J>();
}
m["inputs"]["sources"]=sources;
m["nodes"]=J::obj();
m["nodes"]["satellites"]=U(cfg.s.size());
m["nodes"]["ground"]=U(ground.size());
m["nodes"]["total"]=U(cfg.s.size()+ground.size());
m["products"]=products;
m["warnings"]=J::arr();
m["warnings"].a.emplace_back("MVP: run_id is not normative canonical; package buffers; see IMPLEMENTATION_STATUS.md");
if(backend=="auto")m["warnings"].a.emplace_back("CUDA unavailable: selected cpu");
if(o.m.at("orbit-dir")!="none"&&o.explicitkeys.count("orbit-program"))m["warnings"].a.emplace_back("ignored_orbit_program");
atomic(out/"manifest.json",m.dump());
std::ostringstream csv;
csv<<"source_file,graph_file,M,directed_edges\n";
for(auto&q:products.a)csv<<q.at("source_file").str()<<','<<q.at("graph_file").str()<<','<<q.at("M").u()<<','<<q.at("directed_edges").u()<<'\n';
atomic(out/"manifest.csv",csv.str());
atomic(out/"run.json",m["run"].dump());
atomic(out/"run.log",std::string("SATC_INTEGRITY_OK GRAPH_INTEGRITY_OK SOURCE_VERIFIED\n"));
updatesuccess(out,paths.front().second,out/fs::u8path(products.a.front().at("graph_file").str()));
std::cout<<"run="<<pathtext(out)<<" samples="<<r.M<<" packages="<<paths.size()<<" graphs="<<paths.size()<<" directed_edges="<<edgecount<<'\n';
return out;
}
fs::path demoprog(){
auto p=executable().parent_path()/"satdemo";
#ifdef _WIN32
p+=".exe";
#endif
return p;
}
std::string csvquote(const std::string&s){
std::string t="\"";
for(char c:s){
if(c=='"')t+='"';
t+=c;
}
return t+'"';
}
fs::path csvout(const fs::path&input,const fs::path&explicitout){
if(!explicitout.empty()){
need(!fs::exists(explicitout),1,"csv","explicit output exists");
return explicitout;
}
auto p=input;
p.replace_extension(".csv");
if(!fs::exists(p))return p;
for(U i=1;;++i){
std::ostringstream n;
n<<pathtext(input.stem())<<'_'<<std::setfill('0')<<std::setw(6)<<i<<".csv";
auto q=input.parent_path()/n.str();
if(!fs::exists(q))return q;
}
}
void csvorbit(const Orbit&o,const fs::path&p){
auto part=p;
part+=".part";
try{
std::ofstream f(part,std::ios::binary);
need(bool(f),3,"csv","open");
f<<"t_ns,global_id,name,status,x,y,z,vx,vy,vz\n";
for(U j=0;j<o.M;++j){
safe();
I t=o.t0+I(j)*o.step;
for(U i=0;i<o.N;++i){
auto&s=o.s[size_t(i)];
auto y=o.at(j,i);
f<<t<<','<<s.gid<<','<<csvquote(s.name)<<','<<status(s,t);
for(auto v:y.q)f<<','<<(std::isfinite(v)?number(v):"nan");
f<<'\n';
}
}
f.flush();
need(bool(f),3,"csv","write");
f.close();
safe();
fs::rename(part,p);
}
catch(...){
std::error_code ec;
fs::remove(part,ec);
throw;
}
}
void csvgraph(const Graph&g,const fs::path&p){
auto part=p;
part+=".part";
try{
std::ofstream f(part,std::ios::binary);
need(bool(f),3,"csv","open");
f<<"t_ns,src,dst,delay_us,bandwidth_bps,distance_m,kind\n";
for(U j=0;j<g.M;++j){
safe();
for(auto&e:g.at(j))f<<g.t0+I(j)*g.step<<','<<e.src<<','<<e.dst<<','<<e.delay<<','<<e.bw<<','<<number(e.d)<<','<<e.kind<<'\n';
}
f.flush();
need(bool(f),3,"csv","write");
f.close();
safe();
fs::rename(part,p);
}
catch(...){
std::error_code ec;
fs::remove(part,ec);
throw;
}
}
void reader(const std::string&app,int argc,char**argv){
std::string command="info";
fs::path input,out,source;
int i=1;
if(i<argc&&std::string(argv[i]).rfind("--",0)!=0){
command=argv[i++];
need(command=="info"||command=="verify"||command=="csv",1,"reader","subcommand info|verify|csv");
}
std::vector<std::string>pos;
for(;i<argc;++i){
std::string a=argv[i];
if(a=="--source-orbit"){
need(app=="netread"&&source.empty()&&i+1<argc,1,"reader","source option");
source=fs::u8path(argv[++i]);
}
else{
need(a.rfind("--",0)!=0,1,"reader","unknown option");
pos.push_back(a);
}
}
need(pos.size()<=2&&(command=="csv"||pos.size()<=1),1,"reader","too many positional arguments");
if(!pos.empty())input=fs::u8path(pos[0]);
if(pos.size()==2)out=fs::u8path(pos[1]);
if(input.empty()){
auto choose=[&](){
auto j=json(readtext(home()/"last_success.json"));
auto p=fs::u8path(j.at(app=="satread"?"orbit":"graph").str());
if(app=="satread")readorbit(p,268435456);
else graphread(p,268435456);
return p;
}
;
try{
input=choose();
}
catch(...){
child(demoprog(),{
}
);
input=choose();
}
}
if(app=="satread"){
auto o=readorbit(input,268435456);
if(command=="csv"){
auto p=csvout(input,out);
csvorbit(o,p);
std::cout<<pathtext(p)<<'\n';
}
else if(command=="verify")std::cout<<"SATC_INTEGRITY_OK\n";
else{
J j=J::obj();
j["verified"]=true;
j["format"]="SATC";
j["N"]=o.N;
j["Ns"]=o.Ns;
j["M"]=o.M;
j["t0_ns"]=o.t0;
j["step_ns"]=o.step;
j["frame"]=int(o.frame);
std::cout<<j.dump()<<'\n';
}
}
else{
auto g=graphread(input,268435456,source);
if(command=="csv"){
auto p=csvout(input,out);
csvgraph(g,p);
std::cout<<pathtext(p)<<'\n';
}
else if(command=="verify")std::cout<<(source.empty()?"GRAPH_INTEGRITY_OK":"SOURCE_VERIFIED")<<'\n';
else{
J j=J::obj();
j["verified"]=true;
j["format"]="SATNET";
j["nodes"]=g.N;
j["M"]=g.M;
j["directed_edges"]=g.total;
j["source_verified"]=!source.empty();
std::cout<<j.dump()<<'\n';
}
}
}
void verifyrun(const fs::path&out,U budget){
auto m=json(readtext(out/"manifest.json"));
need(m.at("format").str()=="SATNET-1.1",7,"run","network manifest");
for(auto&q:m.at("products").a){
auto source=out/fs::u8path(q.at("source_file").str()),graph=out/fs::u8path(q.at("graph_file").str()),link=out/fs::u8path(q.at("links_file").str());
need(hex(filesha(source))==q.at("source_sha256").str()&&hex(filesha(graph))==q.at("graph_sha256").str()&&hex(filesha(link))==q.at("links_sha256").str(),7,"run","manifest file SHA mismatch");
graphread(graph,budget,source);
}
}
void demo(int argc,char**argv){
auto o=options("satnet",argc,argv);
for(auto&k:o.explicitkeys)need(k=="output"||k=="threads"||k=="help"||k=="version",1,"satdemo","unsupported option "+k);
if(o.m.count("help")){
std::cout<<"satdemo [--output auto|DIR] [--threads 2]\n";
return;
}
if(o.m.count("version")){
std::cout<<"satdemo 0.1.0-mvp\n";
return;
}
fs::path out=newrun(o.m.at("output"));
auto prog=executable().parent_path()/"satnet";
#ifdef _WIN32
prog+=".exe";
#endif
child(prog,{
"--output",pathtext(out),"--threads",o.m.at("threads")}
);
verifyrun(out,268435456);
auto m=json(readtext(out/"manifest.json"));
J sum=J::obj();
sum["verified"]=true;
if (m.o.count("case_name")) sum["case_name"] = m.at("case_name");
if (m.o.count("rf_parameters")) sum["rf_parameters"] = m.at("rf_parameters");
sum["implementation_status"]="MVP_NOT_FULL_V3_4_ACCEPTANCE";
sum["run_id"]=m.at("run").at("run_id");
sum["packages"]=U(m.at("products").a.size());
U M=0,E=0;
for(auto&q:m.at("products").a){
M+=q.at("M").u();
E+=q.at("directed_edges").u();
}
sum["slices"]=M;
sum["directed_edges"]=E;
sum["numeric_accuracy_certified"]=false;
atomic(out/"summary.json",sum.dump());
auto&first=m.at("products").a.front();
updatesuccess(out,out/fs::u8path(first.at("source_file").str()),out/fs::u8path(first.at("graph_file").str()));
std::cout<<sum.dump()<<'\n';
}
void selftest(){
need(crc(reinterpret_cast<const uint8_t*>("123456789"),9)==0xe3069283,5,"test","crc");
need(hex(sha("abc"))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",5,"test","SHA");
need(hex(sha(""))=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",5,"test","empty SHA");
need(seconds("0.0000000005")==0&&seconds("0.0000000015")==2&&seconds("1e-3")==1000000,5,"test","rounding");
need(iso(utc("2026-01-01T00:00:00.123456789Z"))=="2026-01-01T00:00:00.123456789Z",5,"test","utc");
need(bandwidth(UINT64_MAX,1)==UINT64_MAX&&bandwidth(UINT64_MAX,.5)==UINT64_MAX/2,5,"test","bandwidth");
need(json("{\"x\":18446744073709551615}").at("x").u()==UINT64_MAX,5,"test","u64 json");
bool rejected=false;
try{
json("{\"a\":1,\"a\":2}");
}
catch(const Err&){
rejected=true;
}
need(rejected,5,"test","duplicate JSON");
std::cout<<"SELFTEST_OK\n";
}
int mainapp(const std::string&app,int argc,char**argv){
std::signal(SIGINT,signal_handler);
std::signal(SIGTERM,signal_handler);
if(app=="satread"||app=="netread"){
reader(app,argc,argv);
return 0;
}
if(app=="satdemo"){
demo(argc,argv);
return 0;
}
if(app=="orbit_engine")fail(1,"orbit_engine","diagnostic protocol appendix D not implemented in this MVP; library propagation is implemented");
auto o=options(app,argc,argv);
if(o.m.count("selftest")){
selftest();
return 0;
}
if(o.m.count("version")){
std::cout<<app<<" 0.1.0-mvp C++17 DOP853; v3.4 full compliance not claimed\n";
return 0;
}
if(o.m.count("help")){
std::cout<<app<<" 0.1.0-mvp\n";
for(auto&kv:defaults(app))std::cout<<"  --"<<kv.first<<" (default "<<kv.second<<")\n";
std::cout<<"External eop/ephemeris/density CSV, gravity/phys JSON overrides are supported; CUDA and orbit_engine appendix-D diagnostic protocol are unsupported; see docs.\n";
return 0;
}
if(o.m.count("dump-defaults")){
std::cout<<effective(o).dump()<<'\n';
return 0;
}
if(app=="satpred"){
auto out=predict(o);
std::cout<<"run="<<pathtext(out)<<" SATC_INTEGRITY_OK\n";
}
else network(o);
return 0;
}
}
int application(const char*app,int argc,char**argv){
try{
return sp::mainapp(app,argc,argv);
}
catch(const sp::Err&e){
const char*names[]={
"Ok","Arg","Config","Io","Propagation","Internal","Device","Integrity"}
;
std::cerr<<"{\"code\":"<<e.code<<",\"name\":"<<sp::quote(names[e.code])<<",\"where\":"<<sp::quote(e.where)<<",\"message\":"<<sp::quote(e.what())<<",\"line\":-1,\"col\":-1}\n"<<names[e.code]<<": "<<e.what()<<'\n';
return e.code;
}
catch(const std::filesystem::filesystem_error&e){
std::cerr<<"{\"code\":3,\"name\":\"Io\",\"where\":\"filesystem\",\"message\":"<<sp::quote(e.what())<<",\"line\":-1,\"col\":-1}\nIo: "<<e.what()<<'\n';
return 3;
}
catch(const std::exception&e){
std::cerr<<"{\"code\":5,\"name\":\"Internal\",\"where\":\"main\",\"message\":"<<sp::quote(e.what())<<",\"line\":-1,\"col\":-1}\nInternal: "<<e.what()<<'\n';
return 5;
}
}

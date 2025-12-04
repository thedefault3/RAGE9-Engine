
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <functional>
#include <cmath>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <deque>

using namespace std;

// ------------------------------ Utilities ----------------------------------
using TimePoint = chrono::steady_clock::time_point;
using ms = chrono::duration<double, milli>;

static double nowMillis() {
    return chrono::duration_cast<ms>(chrono::steady_clock::now().time_since_epoch()).count();
}

#define LOGI(fmt, ...) do { fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOGW(fmt, ...) do { fprintf(stdout, "[WARN] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOGE(fmt, ...) do { fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); } while(0)

static string readFileAll(const string &path) {
    ifstream ifs(path, ios::in | ios::binary);
    if(!ifs) return string();
    stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// ------------------------------ Config ------------------------------------
class Config {
public:
    bool load(const string &path) {
        string txt = readFileAll(path);
        if (txt.empty()) return false;
        istringstream iss(txt);
        string line;
        while (getline(iss, line)) {
            // trim
            auto trim = [](string s) {
                size_t a = s.find_first_not_of(" \t\r\n");
                if (a==string::npos) return string();
                size_t b = s.find_last_not_of(" \t\r\n");
                return s.substr(a,b-a+1);
            };
            line = trim(line);
            if (line.empty() || line[0]=='#') continue;
            size_t eq = line.find('=');
            if (eq==string::npos) continue;
            string k = trim(line.substr(0,eq));
            string v = trim(line.substr(eq+1));
            data[k]=v;
        }
        return true;
    }
    string get(const string &k, const string &def="") const { auto it=data.find(k); return it==data.end()?def:it->second; }
    int getInt(const string &k, int def=0) const { auto s=get(k); if(s.empty()) return def; return stoi(s); }
    float getFloat(const string &k, float def=0.0f) const { auto s=get(k); if(s.empty()) return def; return stof(s); }
private:
    unordered_map<string,string> data;
};

// ------------------------------ Timing / Profiler --------------------------
struct Counter {
    double lastUpdated = 0;
    int samples = 0;
    double sum = 0;
    void add(double v) { sum += v; samples++; lastUpdated = nowMillis(); }
    double avg() const { return samples? (sum/samples) : 0.0; }
    void reset() { samples = 0; sum = 0; }
};

// ------------------------------ Resources ---------------------------------
struct Texture {
    SDL_Texture* tex = nullptr;
    int w=0,h=0;
    Texture(SDL_Texture* t=nullptr,int ww=0,int hh=0):tex(t),w(ww),h(hh){}
    ~Texture(){ if(tex) SDL_DestroyTexture(tex); }
};

struct Sound {
    Mix_Chunk* chunk = nullptr;
    Mix_Music* music = nullptr;
    ~Sound(){ if(chunk) Mix_FreeChunk(chunk); if(music) Mix_FreeMusic(music); }
};

class ResourceManager {
public:
    ResourceManager(SDL_Renderer* r):renderer(r){}
    ~ResourceManager(){ textures.clear(); sounds.clear(); }

    shared_ptr<Texture> loadTexture(const string &id, const string &path) {
        if (textures.count(id)) return textures[id];
        SDL_Surface* surf = IMG_Load(path.c_str());
        if (!surf) { LOGW("Failed to load texture %s: %s", path.c_str(), IMG_GetError()); return nullptr; }
        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
        if (!tex) { LOGE("SDL_CreateTextureFromSurface failed: %s", SDL_GetError()); SDL_FreeSurface(surf); return nullptr; }
        auto t = make_shared<Texture>(tex, surf->w, surf->h);
        SDL_FreeSurface(surf);
        textures[id]=t;
        LOGI("Loaded texture '%s' (%s) %dx%d", id.c_str(), path.c_str(), t->w, t->h);
        return t;
    }

    shared_ptr<Sound> loadSound(const string &id, const string &path, bool music=false) {
        if (!audioEnabled) return nullptr;
        if (sounds.count(id)) return sounds[id];
        auto s = make_shared<Sound>();
        if (music) s->music = Mix_LoadMUS(path.c_str()); else s->chunk = Mix_LoadWAV(path.c_str());
        if ((music && !s->music) || (!music && !s->chunk)) { LOGW("Failed to load sound %s: %s", path.c_str(), Mix_GetError()); return nullptr; }
        sounds[id] = s;
        LOGI("Loaded sound '%s' (%s)", id.c_str(), path.c_str());
        return s;
    }

    shared_ptr<Texture> getTexture(const string &id) { if(textures.count(id)) return textures[id]; return nullptr; }
    shared_ptr<Sound> getSound(const string &id) { if(sounds.count(id)) return sounds[id]; return nullptr; }
    void setAudioEnabled(bool v) { audioEnabled = v; }

private:
    SDL_Renderer* renderer = nullptr;
    unordered_map<string, shared_ptr<Texture>> textures;
    unordered_map<string, shared_ptr<Sound>> sounds;
    bool audioEnabled = true;
};

// ------------------------------ ECS ---------------------------------------
struct Component { virtual ~Component(){} };
struct Transform : public Component { float x=0,y=0,rot=0,sx=1,sy=1; };
struct Sprite : public Component { string tex=""; int sx=0,sy=0,sw=0,sh=0; bool centered=true; float layer=0; };
struct Animation { int frameCount=1; float frameTime=0.1f; bool loop=true; int current=0; double timer=0; }; 
struct AnimatedSprite : public Sprite { Animation anim; };
struct Physics : public Component { float vx=0,vy=0,ax=0,ay=0,mass=1.0f,gravity=900.0f; bool onGround=false; };
struct Collider : public Component { float w=16,h=16,offx=0,offy=0; bool isStatic=false; };
struct Script : public Component { function<void(int,double)> onUpdate; function<void(int)> onStart; };
struct CameraComp : public Component { float lerp=0.12f, zoom=1.0f; };
struct UIComp : public Component { string text=""; int fontID=0; };

class World {
public:
    World():nextId(1){}
    int create() { int id = nextId++; entities.push_back(id); return id; }
    void destroy(int id){ // naive
        entities.erase(remove(entities.begin(), entities.end(), id), entities.end());
        components.erase(id);
    }
    template<typename T>
    void add(int id, const string &name, shared_ptr<T> comp) { components[id][name] = comp; }
    template<typename T>
    shared_ptr<T> get(int id, const string &name) {
        auto it = components.find(id);
        if (it==components.end()) return nullptr;
        auto it2 = it->second.find(name);
        if (it2==it->second.end()) return nullptr;
        return static_pointer_cast<T>(it2->second);
    }
    vector<int>& all() { return entities; }
    unordered_map<int, unordered_map<string, shared_ptr<Component>>> &allComps() { return components; }
private:
    int nextId;
    vector<int> entities;
    unordered_map<int, unordered_map<string, shared_ptr<Component>>> components;
};

// ------------------------------ Input -------------------------------------
struct InputState {
    unordered_map<SDL_Scancode,bool> keys;
    unordered_map<int,bool> mouseButtons;
    int mouseX=0, mouseY=0;
    bool quit=false;
    void update() {
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type==SDL_QUIT) quit=true;
            else if(e.type==SDL_KEYDOWN) keys[e.key.keysym.scancode] = true;
            else if(e.type==SDL_KEYUP) keys[e.key.keysym.scancode] = false;
            else if(e.type==SDL_MOUSEBUTTONDOWN) mouseButtons[e.button.button] = true;
            else if(e.type==SDL_MOUSEBUTTONUP) mouseButtons[e.button.button] = false;
            else if(e.type==SDL_MOUSEMOTION){ mouseX=e.motion.x; mouseY=e.motion.y; }
        }
    }
    bool down(SDL_Scancode s) const { auto it=keys.find(s); return it!=keys.end() && it->second; }
};

// Input mapping allows actions
class InputMap {
public:
    void bind(const string &action, SDL_Scancode key) { map[action]=key; }
    bool actionDown(const InputState &st, const string &action) const {
        auto it = map.find(action);
        if(it==map.end()) return false; return st.down(it->second);
    }
private:
    unordered_map<string,SDL_Scancode> map;
};

// ------------------------------ Tilemap ----------------------------------
class Tilemap {
public:
    bool loadCSV(const string &path) {
        string txt = readFileAll(path);
        if(txt.empty()) return false;
        data.clear(); rows=0; cols=0;
        istringstream iss(txt);
        string line;
        while(getline(iss,line)){
            if(line.empty()) continue;
            vector<int> row;
            istringstream ls(line);
            string cell;
            while(getline(ls,cell,',')){
                int v = atoi(cell.c_str()); row.push_back(v);
            }
            if(cols==0) cols = (int)row.size();
            data.push_back(row);
            rows++;
        }
        return true;
    }
    int get(int r,int c) const { if(r<0||r>=rows||c<0||c>=cols) return 0; return data[r][c]; }
    int rows=0, cols=0;
private:
    vector<vector<int>> data;
};

// ------------------------------ Particles --------------------------------
struct Particle { float x,y,vx,vy,life,age; };
class ParticleSystem {
public:
    ParticleSystem(int maxP=1024) { pool.resize(maxP); for(int i=0;i<maxP;i++) pool[i].age=1e9f; }
    void emit(float x,float y,int n){ for(int i=0;i<n;i++){ int idx=findFree(); if(idx<0) break; auto &p=pool[idx]; p.x=x; p.y=y; float ang = ((float)rand()/RAND_MAX)*6.28318f; float sp = 50 + ((float)rand()/RAND_MAX)*200.0f; p.vx=cosf(ang)*sp; p.vy=sinf(ang)*sp; p.life = 300 + rand()%800; p.age=0; } }
    void update(double dt){ for(auto &p:pool){ if(p.age < p.life){ p.age += dt*1000.0f; p.vy += 300.0f * dt; p.x += p.vx * dt; p.y += p.vy * dt; } } }
    void render(SDL_Renderer* r, float camX, float camY){ for(auto &p:pool){ if(p.age < p.life){ float a = 1.0f - (p.age / p.life); SDL_SetRenderDrawColor(r, (Uint8)(255*a), (Uint8)(180*a), (Uint8)(80*a), 255); SDL_Rect rr = {(int)(p.x - camX), (int)(p.y - camY), 2,2}; SDL_RenderFillRect(r, &rr); } } }
private:
    int findFree(){ for(size_t i=0;i<pool.size();++i) if(pool[i].age >= pool[i].life) return (int)i; return -1; }
    vector<Particle> pool;
};

// ------------------------------ Audio Manager -----------------------------
class AudioManager {
public:
    AudioManager() { Mix_AllocateChannels(32); }
    void playSound(shared_ptr<Sound> s){ if(!s||!s->chunk) return; Mix_PlayChannel(-1, s->chunk, 0); }
    void playMusic(shared_ptr<Sound> s){ if(!s||!s->music) return; Mix_PlayMusic(s->music, -1); }
    void stopMusic(){ Mix_HaltMusic(); }
};

// ------------------------------ Renderer Utilities ------------------------
static void drawRect(SDL_Renderer* r, int x,int y,int w,int h){ SDL_Rect rr={x,y,w,h}; SDL_RenderFillRect(r,&rr); }

// ------------------------------ Engine ------------------------------------
class Engine {
public:
    Engine(int w=1280,int h=720,const string &title="Advanced Engine") : screenW(w), screenH(h), windowTitle(title) {}
    bool init(){
        if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_TIMER) < 0){ LOGE("SDL_Init failed: %s", SDL_GetError()); return false; }
        int imgFlags = IMG_INIT_PNG|IMG_INIT_JPG; if(!(IMG_Init(imgFlags) & imgFlags)) LOGW("IMG_Init warning: %s", IMG_GetError());
        if(Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0){ LOGW("Mix_OpenAudio failed: %s", Mix_GetError()); audioAvailable=false; }
        window = SDL_CreateWindow(windowTitle.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, screenW, screenH, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if(!window){ LOGE("CreateWindow failed: %s", SDL_GetError()); return false; }
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if(!renderer){ LOGE("CreateRenderer failed: %s", SDL_GetError()); return false; }
        resources = make_unique<ResourceManager>(renderer);
        resources->setAudioEnabled(audioAvailable);
        world = make_unique<World>();
        audio = make_unique<AudioManager>();
        particles = make_unique<ParticleSystem>(2048);
        inputMap.bind("left", SDL_SCANCODE_A); inputMap.bind("left", SDL_SCANCODE_LEFT);
        inputMap.bind("right", SDL_SCANCODE_D); inputMap.bind("right", SDL_SCANCODE_RIGHT);
        inputMap.bind("jump", SDL_SCANCODE_SPACE);
        lastTime = chrono::steady_clock::now();
        LOGI("Engine initialized");
        return true;
    }

    void loadDefaultAssets(){
        // These are optional: if not present, engine still runs
        resources->loadTexture("player", "assets/player.png");
        resources->loadTexture("tiles", "assets/tiles.png");
        resources->loadTexture("font", "assets/font.png");
        if(audioAvailable){ resources->loadSound("bg", "assets/bg.ogg", true); resources->loadSound("jump", "assets/jump.wav", false); }
    }

    void createDemoScene(){
        // Tilemap ground
        int tileW=64,tileH=64;
        for(int cx=0; cx<20; ++cx){
            for(int cy=8; cy<12; ++cy){
                int id = world->create();
                auto t = make_shared<Transform>(); t->x = cx*tileW + tileW/2; t->y = cy*tileH + tileH/2; world->add(id,"transform",t);
                auto s = make_shared<Sprite>(); s->tex = "tiles"; s->sw = tileW; s->sh = tileH; s->centered=true; world->add(id,"sprite",s);
                auto c = make_shared<Collider>(); c->w = tileW; c->h = tileH; c->isStatic = true; world->add(id,"collider",c);
            }
        }
        // Player
        int pid = world->create(); auto pt = make_shared<Transform>(); pt->x=100; pt->y=100; world->add(pid,"transform",pt);
        auto ps = make_shared<AnimatedSprite>(); ps->tex = "player"; ps->sw=48; ps->sh=48; ps->centered=true; ps->anim.frameCount=4; ps->anim.frameTime=0.12f; world->add(pid,"sprite",ps);
        auto ph = make_shared<Physics>(); ph->vx=0; ph->vy=0; world->add(pid,"physics",ph);
        auto pc = make_shared<Collider>(); pc->w=40; pc->h=40; pc->isStatic=false; world->add(pid,"collider",pc);
        auto scr = make_shared<Script>();
        scr->onUpdate = [this, pid](int id,double dt){ // player control
            auto tr = world->get<Transform>(pid,"transform"); auto ph = world->get<Physics>(pid,"physics"); auto spr = world->get<AnimatedSprite>(pid,"sprite"); if(!tr||!ph) return;
            float speed = 240.0f; bool left = input.down(SDL_SCANCODE_LEFT) || input.down(SDL_SCANCODE_A); bool right = input.down(SDL_SCANCODE_RIGHT) || input.down(SDL_SCANCODE_D);
            if(left) ph->vx = -speed; else if(right) ph->vx = speed; else ph->vx = 0;
            if((input.down(SDL_SCANCODE_SPACE) || input.down(SDL_SCANCODE_W)) && ph->onGround){ ph->vy = -420.0f; ph->onGround=false; auto snd=resources->getSound("jump"); if(snd) audio->playSound(snd); }
            // animation
            if(spr){ if(fabs(ph->vx) > 1.0f) spr->anim.frameTime = 0.12f; else spr->anim.frameTime = 0.4f; }
        };
        world->add(pid,"script",scr);

        // Camera
        int camId = world->create(); auto ct = make_shared<Transform>(); ct->x=0; ct->y=0; world->add(camId,"transform",ct); auto cc = make_shared<CameraComp>(); cc->lerp=0.12f; world->add(camId,"camera",cc);

        // Collectible example
        for(int i=0;i<5;i++){ int id = world->create(); auto t = make_shared<Transform>(); t->x = 400 + i*80; t->y = 200; world->add(id,"transform",t); auto s = make_shared<Sprite>(); s->tex = "tiles"; s->sw=32; s->sh=32; world->add(id,"sprite",s); auto c = make_shared<Collider>(); c->w=32; c->h=32; c->isStatic=false; world->add(id,"collider",c); auto scr2 = make_shared<Script>(); scr2->onUpdate = [this,id](int eid,double dt){}; world->add(id,"script",scr2); }

        sceneStarted = true;
        if(audioAvailable){ auto bg = resources->getSound("bg"); if(bg) audio->playMusic(bg); }
    }

    void run(){ running=true; const double fixedDt = 1.0/60.0; const double maxAccum = 0.25; double accumulator=0.0; while(running){ TimePoint frameStart = chrono::steady_clock::now(); input.update(); if(input.quit) running=false; double now = chrono::duration_cast<ms>(chrono::steady_clock::now().time_since_epoch()).count(); chrono::duration<double> frameTime = chrono::steady_clock::now() - lastTime; lastTime = chrono::steady_clock::now(); accumulator += frameTime.count(); if(accumulator > maxAccum) accumulator = maxAccum; while(accumulator >= fixedDt){ fixedUpdate(fixedDt); accumulator -= fixedDt; } render(); // frame cap if not vsync
            if(!vsync){ TimePoint frameEnd = chrono::steady_clock::now(); chrono::duration<double,milli> elapsed = frameEnd - frameStart; double targetMs = 1000.0/60.0; if(elapsed.count() < targetMs) SDL_Delay((Uint32)(targetMs - elapsed.count())); }
        }
        cleanup(); }

private:
    void fixedUpdate(double dt){ // update scripts, physics integration
        // scripts
        for(auto id : world->all()){
            auto sc = world->get<Script>(id,"script"); if(sc && sc->onUpdate) sc->onUpdate(id, dt); }
        // integrate physics
        for(auto id: world->all()){
            auto ph = world->get<Physics>(id,"physics"); auto tr = world->get<Transform>(id,"transform"); if(ph && tr){ ph->vy += ph->gravity * dt; ph->vx += ph->ax * dt; ph->vy += ph->ay * dt; tr->x += ph->vx * dt; tr->y += ph->vy * dt; } }
        // collision detection/resolution
        collisionSolve();
        // animations
        for(auto id: world->all()){
            auto an = world->get<AnimatedSprite>(id,"sprite"); if(an){ an->anim.timer += dt; if(an->anim.timer >= an->anim.frameTime){ an->anim.timer = 0; an->anim.current = (an->anim.current + 1) % max(1, an->anim.frameCount); } } }
        // update particle system
        particles->update(dt);
    }

    void collisionSolve(){ auto &ents = world->all(); // naive O(N^2)
        for(size_t i=0;i<ents.size();++i){ int a = ents[i]; auto ac = world->get<Collider>(a,"collider"); auto at = world->get<Transform>(a,"transform"); if(!ac||!at) continue; AABB aa = { at->x - ac->w/2.0f + ac->offx, at->y - ac->h/2.0f + ac->offy, ac->w, ac->h };
            for(size_t j=i+1;j<ents.size();++j){ int b = ents[j]; auto bc = world->get<Collider>(b,"collider"); auto bt = world->get<Transform>(b,"transform"); if(!bc||!bt) continue; AABB bb = { bt->x - bc->w/2.0f + bc->offx, bt->y - bc->h/2.0f + bc->offy, bc->w, bc->h };
                if(aabbIntersect(aa,bb)){
                    resolveCollision(a,b,aa,bb);
                }
            }
        }
    }

    struct AABB { float x,y,w,h; };
    static bool aabbIntersect(const AABB &a, const AABB &b){ return !(a.x+a.w < b.x || b.x+b.w < a.x || a.y+a.h < b.y || b.y+b.h < a.y); }

    void resolveCollision(int aid, int bid, AABB &aa, AABB &bb){ auto ac = world->get<Collider>(aid,"collider"); auto at = world->get<Transform>(aid,"transform"); auto ap = world->get<Physics>(aid,"physics"); auto bc = world->get<Collider>(bid,"collider"); auto bt = world->get<Transform>(bid,"transform"); auto bp = world->get<Physics>(bid,"physics"); if(!ac||!at||!bc||!bt) return;
        float axc = aa.x + aa.w*0.5f; float ayc = aa.y + aa.h*0.5f; float bxc = bb.x + bb.w*0.5f; float byc = bb.y + bb.h*0.5f; float dx = bxc - axc; float dy = byc - ayc; float overlapX = (aa.w+bb.w)/2.0f - fabs(dx); float overlapY = (aa.h+bb.h)/2.0f - fabs(dy);
        if(overlapX < overlapY){ // resolve in X
            float sign = dx>0?1.0f:-1.0f; if(!ac->isStatic && !bc->isStatic){ at->x -= sign * overlapX*0.5f; bt->x += sign * overlapX*0.5f; } else if(!ac->isStatic){ at->x -= sign * overlapX; } else if(!bc->isStatic){ bt->x += sign * overlapX; }
            if(ap) ap->vx = 0; if(bp) bp->vx = 0;
        } else { // resolve in Y
            float sign = dy>0?1.0f:-1.0f; if(!ac->isStatic && !bc->isStatic){ at->y -= sign * overlapY*0.5f; bt->y += sign * overlapY*0.5f; } else if(!ac->isStatic){ at->y -= sign * overlapY; } else if(!bc->isStatic){ bt->y += sign * overlapY; }
            // set ground flags
            if(ap){ if(sign>0) ap->onGround = true; ap->vy = 0; }
            if(bp){ if(sign<0) bp->onGround = true; bp->vy = 0; }
        }
    }

    void render(){ // clear
        SDL_SetRenderDrawColor(renderer, 18, 20, 24, 255); SDL_RenderClear(renderer);
        // camera transform
        computeCamera();
        // render sprites (no sorting for demo)
        for(auto id: world->all()){
            auto sp = world->get<Sprite>(id,"sprite"); auto tr = world->get<Transform>(id,"transform"); if(!sp || !tr) continue; auto tex = resources->getTexture(sp->tex); if(!tex) continue; SDL_Rect src{ sp->sx, sp->sy, sp->sw?sp->sw:tex->w, sp->sh?sp->sh:tex->h };
                int dw = (int)(src.w * tr->sx); int dh = (int)(src.h * tr->sy); SDL_Rect dst{ (int)round(tr->x - camX - (sp->centered?dw/2.0f:0)), (int)round(tr->y - camY - (sp->centered?dh/2.0f:0)), dw, dh };
                SDL_RenderCopyEx(renderer, tex->tex, &src, &dst, tr->rot, nullptr, SDL_FLIP_NONE);
        }
        // particles
        particles->render(renderer, camX, camY);
        // UI: render debug overlay
        renderDebugOverlay();
        SDL_RenderPresent(renderer);
    }

    void computeCamera(){ // follow first entity with physics
        float targetX=0, targetY=0; bool found=false;
        for(auto id: world->all()){ auto p = world->get<Physics>(id,"physics"); auto tr = world->get<Transform>(id,"transform"); if(p && tr){ targetX = tr->x - screenW/2.0f; targetY = tr->y - screenH/2.0f; found=true; break; } }
        if(!found) return; camX += (targetX - camX) * 0.12f; camY += (targetY - camY) * 0.12f; }

    void renderDebugOverlay(){ // simple FPS & stats
        double t = nowMillis(); frameCount++; if(t - lastFPSTime >= 500.0){ fps = (frameCount*1000.0)/(t-lastFPSTime); frameCount=0; lastFPSTime=t; }
        // draw simple overlay box
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0,0,0,160); SDL_Rect box = {8,8,220,80}; SDL_RenderFillRect(renderer,&box);
        // use console logging for details
        LOGI("FPS: %.1f | Entities: %zu | Textures: %zu", fps, world->all().size(), 0);
    }

    void cleanup(){ resources.reset(); world.reset(); audio.reset(); particles.reset(); if(renderer){ SDL_DestroyRenderer(renderer); renderer=nullptr; } if(window){ SDL_DestroyWindow(window); window=nullptr; } Mix_CloseAudio(); IMG_Quit(); SDL_Quit(); }

    int screenW, screenH; string windowTitle; SDL_Window* window=nullptr; SDL_Renderer* renderer=nullptr; unique_ptr<ResourceManager> resources; unique_ptr<World> world; unique_ptr<AudioManager> audio; unique_ptr<ParticleSystem> particles;
    InputState input; InputMap inputMap;
    bool running=false; bool vsync=true; bool audioAvailable=true; bool sceneStarted=false; TimePoint lastTime;
    // camera
    float camX=0, camY=0;
    // debug
    double fps=0; int frameCount=0; double lastFPSTime=nowMillis();
};

// ------------------------------ Main --------------------------------------
int main(int argc, char** argv){ srand((unsigned)time(nullptr)); Engine e(1280,720,"Advanced Engine Demo"); if(!e.init()) return EXIT_FAILURE; e.loadDefaultAssets(); e.createDemoScene(); e.run(); return EXIT_SUCCESS; }


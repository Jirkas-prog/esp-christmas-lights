#pragma once
// Included after publishJson(): all entities belong to the existing light device.
static bool discoverControls(PubSubClient &mqtt) {
    DynamicJsonDocument d(2048);bool ok=true;
    auto begin=[&](const String &id,const String &name,const String &suffix,const String &field) {
        d.clear();d["name"]=name;d["unique_id"]=deviceId+"_"+id;
        d["availability_topic"]=baseTopic+"/availability";
        d["command_topic"]=baseTopic+"/"+suffix;d["state_topic"]=baseTopic+"/config";
        d["value_template"]="{{ value_json."+field+" }}";
        d["retain"]=false;d["qos"]=1;d["entity_category"]="config";
        auto device=d.createNestedObject("device");device.createNestedArray("identifiers").add(deviceId);device["name"]=hostname;
    };
    auto send=[&](const char *domain,const String &id) {
        ok=publishJson(mqtt,"homeassistant/"+String(domain)+"/"+deviceId+"_"+id+"/config",d) && ok;
    };
    auto effectOptions=[&](const String &field) {
        auto options=d.createNestedArray("options");String mapping="{{ {";
        for (unsigned i=0;i<lights::ModeCount;++i) {
            auto mode=lights::Mode(i);options.add(lights::modeLabel(mode));
            if (i) mapping+=",";
            mapping+="'"+String(lights::modeName(mode))+"':'"+lights::modeLabel(mode)+"'";
        }
        d["value_template"]=mapping+"}[value_json."+field+"] }}";
    };
    auto toggle=[&](const String &id,const String &name,const String &suffix,const String &field,const String &key) {
        begin(id,name,suffix,field);
        d["value_template"]="{{ 'ON' if value_json."+field+" else 'OFF' }}";
        d["state_on"]="ON";d["state_off"]="OFF";
        d["payload_on"]="{\""+key+"\":true}";d["payload_off"]="{\""+key+"\":false}";send("switch",id);
    };
    toggle("schedule_enabled","Časový plán","schedule/set","schedule.enabled","enabled");
    toggle("one_percent_off","Při 1 % vypnout","schedule/set","schedule.one_percent_off","one_percent_off");
    toggle("protection_enabled","Ochrana proti zkratu","protection/set","protection.enabled","enabled");
    toggle("latch_fault","Po poruše trvale blokovat","protection/set","protection.latch_fault","latch_fault");
    struct Number {const char *id,*name,*topic,*field,*key,*unit;float min,max,step;};
    for (auto &n: {
        Number{"preset_brightness","Jas při zapnutí","preset/set","on_preset.brightness","brightness","%",2,100,1},
        Number{"night_brightness","Noční mezera: jas (0 = vypnuto)","schedule/night/set","night_gap.brightness","brightness","%",0,100,1},
        Number{"latitude","Zeměpisná šířka","schedule/set","schedule.latitude","latitude","°",-90,90,.001f},
        Number{"longitude","Zeměpisná délka","schedule/set","schedule.longitude","longitude","°",-180,180,.001f},
        Number{"threshold","Práh ochrany","protection/set","protection.threshold_percent","threshold_percent","%",30,95,1},
        Number{"fault_delay","Prodleva ochrany","protection/set","protection.delay_ms","delay_ms","ms",100,10000,100},
        Number{"limit_first","První polovina: ignorovat do jasu","protection/limits/set","protection.limits.first","first","%",0,100,1},
        Number{"limit_second","Druhá polovina: ignorovat do jasu","protection/limits/set","protection.limits.second","second","%",0,100,1},
        Number{"limit_all","Celý řetěz: ignorovat do jasu","protection/limits/set","protection.limits.all","all","%",0,100,1},
        Number{"effect_interval","Interval střídání / prolínání","set","interval_ms","interval_ms","ms",100,10000,1}}) {
        begin(n.id,n.name,n.topic,n.field);
        if (!strcmp(n.id,"effect_interval")) d["state_topic"]=baseTopic+"/state";
        d["command_template"]="{\""+String(n.key)+"\":{{ value | "+(n.step<1?"float":"int")+" }}}";
        d["min"]=n.min;d["max"]=n.max;d["step"]=n.step;d["mode"]="box";d["unit_of_measurement"]=n.unit;send("number",n.id);
    }
    begin("timezone","Časové pásmo","schedule/set","schedule.timezone");
    auto zones=d.createNestedArray("options");for (auto &z:schedule::Zones) zones.add(z.name);
    d["command_template"]="{\"timezone\":\"{{ value }}\"}";send("select","timezone");
    begin("preset_effect","Režim při zapnutí","preset/set","on_preset.effect");
    effectOptions("on_preset.effect");
    d["command_template"]="{\"effect\":\"{{ value }}\"}";send("select","preset_effect");
    begin("night_effect","Noční mezera: režim","schedule/night/set","night_gap.effect");
    effectOptions("night_gap.effect");
    d["command_template"]="{\"effect\":\"{{ value }}\"}";send("select","night_effect");
    for (unsigned i=0;i<2;++i) {
        String id="window_"+String(i),label=i?"Ráno: ":"Večer: ";
        String path="schedule.windows["+String(i)+"]",topic="schedule/window/"+String(i)+"/set";
        String preset=i?"morning":"evening",presetPath="window_presets."+preset;
        begin(id+"_brightness",label+"jas (0 = vypnuto)","schedule/presets/set",presetPath+".brightness");
        d["command_template"]="{\""+preset+"\":{\"brightness\":{{ value | int }}}}";
        d["min"]=0;d["max"]=100;d["step"]=1;d["mode"]="box";d["unit_of_measurement"]="%";send("number",id+"_brightness");
        begin(id+"_effect",label+"režim","schedule/presets/set",presetPath+".effect");
        effectOptions(presetPath+".effect");
        d["command_template"]="{\""+preset+"\":{\"effect\":\"{{ value }}\"}}";send("select",id+"_effect");
        toggle(id+"_enabled",label+"používat interval",topic,path+".enabled","enabled");
        toggle(id+"_overnight",label+"přes půlnoc",topic,path+".overnight","overnight");
        for (const char *end:{"start","end"}) {
            String pointId=id+"_"+end,field=path+"."+end,name=label+(!strcmp(end,"start")?"zapnout":"vypnout");
            begin(pointId+"_kind",name+" podle",topic,field+".kind");
            auto options=d.createNestedArray("options");options.add("Pevný čas");options.add("Východ slunce");options.add("Západ slunce");
            d["value_template"]="{{ {'fixed':'Pevný čas','sunrise':'Východ slunce','sunset':'Západ slunce'}[value_json."+field+".kind] }}";
            d["command_template"]="{\""+String(end)+"\":{\"kind\":\"{{ {'Pevný čas':'fixed','Východ slunce':'sunrise','Západ slunce':'sunset'}[value] }}\"}}";
            send("select",pointId+"_kind");
            begin(pointId+"_offset",name+" posun před/po slunci",topic,field+".offset");
            d["command_template"]="{\""+String(end)+"\":{\"offset\":{{ value | int }}}}";
            d["min"]=-720;d["max"]=720;d["step"]=1;d["mode"]="box";d["unit_of_measurement"]="min";send("number",pointId+"_offset");
            begin(pointId+"_time",name+" pevný čas",topic,field+".minute");
            d["value_template"]="{{ '%02d:%02d:00' | format((value_json."+field+".minute | int) // 60, (value_json."+field+".minute | int) % 60) }}";
            d["command_template"]="{\""+String(end)+"\":{\"minute\":{{ (value.split(':')[0] | int) * 60 + (value.split(':')[1] | int) }}}}";send("time",pointId+"_time");
        }
    }
    struct Button {const char *id,*name,*topic,*payload;};
    for (auto &b:{
        Button{"resume","Vrátit řízení plánu","schedule/action","{\"action\":\"resume\"}"},
        Button{"calibrate","Spustit automatickou kalibraci","protection/action","{\"action\":\"calibrate\"}"},
        Button{"cancel_calibration","Zrušit kalibraci","protection/action","{\"action\":\"cancel\"}"},
        Button{"clear_fault","Potvrdit poruchu","protection/action","{\"action\":\"clear\"}"},
        Button{"automatic_limits","Obnovit automatické hranice ochrany","protection/limits/set","{\"automatic\":true}"},
        Button{"restart","Restart (Restartovat desku)","system/action","{\"action\":\"restart\"}"}}) {
        begin(b.id,b.name,b.topic,"");d.remove("state_topic");d.remove("value_template");
        if (!strcmp(b.id,"restart")) {d["device_class"]="restart";d["enabled_by_default"]=true;}
        d["payload_press"]=b.payload;send("button",b.id);
    }
    for (auto key:{"thermal_warning","fault"}) {
        String id="protection_"+String(key);
        begin(id,!strcmp(key,"fault")?"Ochrana vypnula světlo":"Překročena tepelná mez 2 W","","");
        d.remove("command_topic");d.remove("retain");d["entity_category"]="diagnostic";d["device_class"]="problem";
        d["state_topic"]=baseTopic+"/state";d["payload_on"]="ON";d["payload_off"]="OFF";
        d["value_template"]="{{ 'ON' if value_json.protection."+String(key)+" else 'OFF' }}";send("binary_sensor",id);
    }
    begin("calibration_progress","Průběh kalibrace","","protection.progress");
    d.remove("command_topic");d.remove("retain");d["entity_category"]="diagnostic";
    d["state_topic"]=baseTopic+"/state";d["unit_of_measurement"]="%";send("sensor","calibration_progress");
    return ok;
}

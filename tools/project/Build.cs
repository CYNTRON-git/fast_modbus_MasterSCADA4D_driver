using System;
using System.IO;
using System.Linq;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
public static partial class StartupHook{
    static IEnumerable<object> Children(object o)=>((IEnumerable)Get(o,"Childs")!).Cast<object>();
    static string Ref(object o)=>Get(o,"RefName")?.ToString()??"";
    static string Kind(object o)=>Get(Get(o,"ItemType"),"TypeName")?.ToString()??"";
    static void Set(object o,string key,object value)=>Call(o,"SetProperty",key,value);
    static object Child(object p,string r)=>Children(p).Single(c=>Ref(c)==r);
    static object Insert(object p,object tm,string type,string name)=>Call(Get(p,"ManagementService")!,"InsertChild",Call(tm,"GetItemType",type),name,name,null)!;
    static object Folder(object p,string name)=>Children(p).FirstOrDefault(c=>Ref(c)==name)??Call(Get(p,"ManagementService")!,"GetOrInsertChild",name,true)!;
    static object Setting(object parent,object tm,string name,string type,string initial){
        var s=Insert(parent,tm,"MasterSCADA.System.Setting",name);
        Set(s,"MasterSCADA.Var.Type",type);Set(s,"MasterSCADA.Var.InitialValue",initial);return s;
    }
    static void Build(object project,object tm){
        Assembly.LoadFrom(Path.Combine(AppContext.BaseDirectory,"MasterSCADA.Scripts.Model.dll"));
        var all=((IEnumerable)Call(project,"GetAllItems")!).Cast<object>().ToList();
        var libraries=Service(Child(Get(project,"RootItem")!,"LibraryRoot"),"InSAT.Framework.DbStore.Services.Item.Library.IncludedLibrariesService");
        var local=Call(libraries,"InsertLibrary","FastModbusBenchTypes_"+DateTime.UtcNow.ToString("yyyyMMdd_HHmmss"),Call(tm,"GetProjectType","MasterSCADA.LibraryProject"))!;
        Log("USER LIBRARY "+Ref(local));
        var bp=all.Single(o=>Kind(o)=="MasterSCADA.System.ProtocolType"&&Ref(o)=="BaseFBProtocol");
        var proto=Call(Get(Folder(local,"Protocols"),"ManagementService")!,"InsertInheritedCopy",bp)!;
        Set(proto,"DisplayName","FastModbus");Set(proto,"RefName","FastModbus");Set(proto,"SubTypeName","FastModbus");
        Set(proto,"MasterSCADA.Category","FastModbus");Set(proto,"MasterSCADA.ParentCategory","IOPorts");Set(proto,"MasterSCADA.Program.IsExternal",true);
        var ps=Folder(proto,"Settings");
        var inputs=new[]{("PortName","STRING","tcp://127.0.0.1:15502"),("BaudRate","DINT","115200"),("Parity","DINT","0"),("StopBits","DINT","1"),("DataBits","DINT","8"),("ResponseTimeoutMs","DINT","200"),("InterFrameDelayMs","DINT","5"),("EventPollIntervalMs","DINT","100"),("FallbackPollPeriodMs","DINT","1000"),("HealthPollPeriodMs","DINT","5000"),("DeviceTimeoutMs","DINT","15000"),("MaxTransactionsPerCycle","DINT","8"),("MaxCycleMs","DINT","100"),("EnableFastModbus","BOOL","True"),("AutoScan","BOOL","False"),("AllowWrites","BOOL","False")};
        foreach(var (n,t,v) in inputs)Setting(ps,tm,n,t,v);
        foreach(var n in new[]{"Requests","Responses","Timeouts","BadFrames","EventPackets","EventValues","DuplicatePackets","ReadRequests","WriteRequests","WriteErrors","RejectedSubscriptions","ScannedDevices"}){
            var a=Insert(ps,tm,"MasterSCADA.System.Setting",n);Set(a,"MasterSCADA.Var.Type","LINT");Set(a,"MasterSCADA.Var.AccessType",1);
        }
        var bm=all.Where(o=>Kind(o)=="MasterSCADA.System.IOModuleType"&&Ref(o)=="Базовый модуль").ToList();
        if(bm.Count!=1){foreach(var m in all.Where(o=>Kind(o)=="MasterSCADA.System.IOModuleType"))Log("MODULE CANDIDATE "+Get(m,"Id")+" "+Ref(m));throw new Exception("Base module count="+bm.Count);}
        var module=Call(Get(Folder(local,"IOModules"),"ManagementService")!,"InsertInheritedCopy",bm[0])!;
        Set(module,"DisplayName","MR02m_12AI_FMB");Set(module,"RefName","MR02m_12AI_FMB");Set(module,"MasterSCADA.ParentCategory","FastModbus");
        var ms=Folder(module,"Settings");
        Setting(ms,tm,"ModbusAddress","DINT","12");Setting(ms,tm,"SerialNumber","DINT","235561385");Setting(ms,tm,"UseSerial","BOOL","False");
        Setting(ms,tm,"GroupEventRanges","BOOL","True");
        var model=Activator.CreateInstance(Find("MasterSCADA.Scripts.Model.IOModuleItem"),BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance,null,new object[]{module},null)!;
        var channels=new List<(string name,int reg,int type,double scale,int priority)>();
        for(int i=0;i<12;i++)channels.Add(("AI"+(i+1),403+7*i,0,1,2));
        channels.AddRange(new[]{("Uptime",105,2,1.0,0),("VDD",123,0,0.01,0),("MCUTemperature",124,1,0.1,0),("Health",65519,0,1.0,0)});
        foreach(var c in channels){
            var ch=Get(Call(model,"AddChannelAI",c.name),"ItemInstance")!;
            Set(ch,"DisplayName",c.name);Set(ch,"RefName",c.name);
            var input=Child(ch,"Input");Set(input,"MasterSCADA.Var.Type","LREAL");Set(ch,"MasterSCADA.Channel.UseScaling",false);
            var cs=Children(ch).Single(o=>Kind(o)=="MasterSCADA.System.ChannelSettings");
            Setting(cs,tm,"RegType","DINT","3");Setting(cs,tm,"RegAddr","DINT",c.reg.ToString());Setting(cs,tm,"Priority","DINT",c.priority.ToString());
            Setting(cs,tm,"ValueType","DINT",c.type.ToString());Setting(cs,tm,"WordOrder","DINT","0");Setting(cs,tm,"Scale","LREAL",c.scale.ToString(System.Globalization.CultureInfo.InvariantCulture));
            Setting(cs,tm,"Offset","LREAL","0");Setting(cs,tm,"Deadband","LREAL","0");Setting(cs,tm,"MinIntervalMs","DINT","0");Setting(cs,tm,"BurstWindowMs","DINT","0");Setting(cs,tm,"BurstMaxEvents","DINT","0");
        }
        var libraryService=Service(local,"InSAT.Framework.Core.Services.Item.Library.ILibraryReferenceService");
        if(Get(libraryService,"CanSaveChanges") is not true)throw new Exception("New library is not writable");
        Call(libraryService,"SaveLibraryChanges",new object?[]{null});
        var findStore=libraries.GetType().GetMethods().Single(m=>m.Name=="FindStoreService"&&m.GetParameters().Length==2&&m.GetParameters()[0].ParameterType.IsInstanceOfType(local));
        var libraryStore=Get(findStore.Invoke(libraries,new object[]{local,false}),"StoreService")!;
        var libOut=Path.Combine(Path.GetTempPath(),"ms4d_fast_modbus_20261122","native-libraries",Ref(local)+".zip");
        Directory.CreateDirectory(Path.GetDirectoryName(libOut)!);
        ExportDatabase(libraryStore,libOut);
        File.WriteAllText(Path.Combine(Path.GetTempPath(),"ms4d_fast_modbus_20261122","library-export-path.txt"),libOut);
        var root=Activator.CreateInstance(Find("MasterSCADA.Scripts.Model.RootItem"),Get(project,"RootItem"))!;
        var arm=Call(Get(root,"System")!,"AddARM","FMB_Bench")!;
        var armNode=Get(arm,"ItemInstance")!;
        Set(armNode,"DisplayName","FMB_Bench");Set(armNode,"RefName","FMB_Bench");
        // The default ARM service protocols are fixed definitions; only FastModbus has field modules.
        foreach(var item in Walk(armNode))if(Ref(item)=="AutoStartPLC")Set(item,"MasterSCADA.Var.InitialValue","False");
        var instance=Call(arm,"AddProtocol","FastModbus")!;
        var instNode=Get(instance,"ItemInstance")!;Set(instNode,"SubTypeName","FastModbus");
        Set(instNode,"DisplayName","FastModbus");Set(instNode,"RefName","FastModbus");
        var device=Call(instance,"AddModule","MR02m_12AI_FMB")!;
        var deviceNode=Get(device,"ItemInstance")!;Set(deviceNode,"DisplayName","MR12AI_12");Set(deviceNode,"RefName","MR12AI_12");
        var effective=new Dictionary<string,object?>();
        foreach(var node in Walk(instNode).Where(n=>Kind(n)=="MasterSCADA.System.Setting"))effective[Get(node,"Id")!.ToString()!]=new {name=Ref(node),initial=Call(node,"GetPropertyValue","MasterSCADA.Var.InitialValue"),path=Get(node,"FullRefName")};
        File.WriteAllText(Path.Combine(Path.GetTempPath(),"ms4d_fast_modbus_20261122","native-effective-settings.json"),System.Text.Json.JsonSerializer.Serialize(effective));
        Log("BUILT controller="+Get(armNode,"Id")+" protocol="+Get(instNode,"Id")+" module="+Get(device,"Id"));
        var errors=((IEnumerable)Call(Get(project,"ManagementService")!,"CheckProject")!).Cast<object>().ToList();
        foreach(var e in errors)Log("CHECK "+e.GetType()+" "+e);
        Log("CHECK COUNT "+errors.Count);
        if(errors.Count!=0)throw new Exception("Native model check failed");
        File.WriteAllText(Path.Combine(Path.GetTempPath(),"ms4d_fast_modbus_20261122","project-manifest.txt"),"projectId="+Get(project,"ProjectId")+"\ncontroller="+Get(armNode,"Id")+"\nprotocol="+Get(instNode,"Id")+"\nmodule="+Get(device,"Id")+"\nAllowWrites=False\nPort=tcp://127.0.0.1:15502\nHardwareValidated=False\n");
    }
    static IEnumerable<object> Walk(object item){yield return item;foreach(var c in Children(item))foreach(var v in Walk(c))yield return v;}
}

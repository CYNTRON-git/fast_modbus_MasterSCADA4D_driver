using System;
using System.IO;
using System.Linq;
using System.Collections;
using System.Reflection;
using System.Threading;
using System.Windows;
public static partial class StartupHook{
    static Timer? timer;static int busy,done,ticks;
    static object? ownedProject;
    static string LogPath=>Environment.GetEnvironmentVariable("FMB_PROJECT_LOG")??"";
    static void Log(string s){if(LogPath.Length>0)File.AppendAllText(LogPath,DateTime.UtcNow.ToString("O")+" "+s+Environment.NewLine);}
    public static void Initialize(){
        if(System.Diagnostics.Process.GetCurrentProcess().ProcessName!="ms4d"||LogPath.Length==0)return;
        Log("FMB project hook, no controller or runtime operations");
        timer=new Timer(Poll,null,2000,1000);
    }
    static object? Get(object? o,string n)=>o?.GetType().GetProperty(n,BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance|BindingFlags.Static)?.GetValue(o);
    static Type Find(string n)=>AppDomain.CurrentDomain.GetAssemblies().Select(a=>a.GetType(n)).First(t=>t!=null)!;
    static object? Call(object o,string n,params object?[] args){
        var m=o.GetType().GetMethods(BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance).First(x=>x.Name==n&&x.GetParameters().Length==args.Length&&x.GetParameters().Select((p,i)=>args[i]==null||p.ParameterType.IsInstanceOfType(args[i])).All(b=>b));
        return m.Invoke(o,args);
    }
    static void Poll(object? state){
        if(Volatile.Read(ref done)!=0||Interlocked.Exchange(ref busy,1)!=0)return;
        try{
            if(++ticks>90){Log("TIMEOUT");Volatile.Write(ref done,1);Application.Current?.Dispatcher.BeginInvoke(new Action(()=>Application.Current.Shutdown()));return;}
            if(Application.Current==null)return;
            Application.Current.Dispatcher.BeginInvoke(new Action(Run));
        }finally{Volatile.Write(ref busy,0);}
    }
    static void Run(){
        if(Volatile.Read(ref done)!=0)return;
        try{
            var tm=Find("InSAT.Framework.Core.DataModel.Types.TypesManager");
            var inited=tm.GetProperty("InstanceInited")!.GetValue(null);
            if(inited is not true){if(ticks%10==0)Log("Types not initialized");return;}
            Volatile.Write(ref done,1);
            var manager=tm.GetProperty("Instance")!.GetValue(null)!;
            Log("ConfigName="+Get(manager,"ConfigName")+" ConfigPath="+Get(manager,"ConfigPath"));
            foreach(var p in (IEnumerable)Get(manager,"ProjectTemplates")!)Log("TEMPLATE "+Get(p,"TypeName")+" "+Get(p,"DisplayName"));
            foreach(var p in (IEnumerable)Get(manager,"StoreTypes")!)Log("STORE "+Get(p,"TypeName"));
            var ap=Find("InSAT.Framework.UI.Settings.AppStatic").GetProperty("Instance")!.GetValue(null)!;
            Log("SelectedConfig="+Get(ap,"SelectedConfig"));
            var compilePath=Environment.GetEnvironmentVariable("FMB_COMPILE_PROJECT");
            if(!string.IsNullOrEmpty(compilePath)){
                var allowed=Path.GetFullPath(Path.Combine(Path.GetTempPath(),"ms4d_fast_modbus_20261122","project"))+Path.DirectorySeparatorChar;
                var path=Path.GetFullPath(compilePath);
                if(!path.StartsWith(allowed,StringComparison.OrdinalIgnoreCase)||Path.GetFileName(path)!="FastModbusBench")throw new Exception("Compile target outside task project directory");
                ownedProject=Application.Current.Windows.Cast<Window>().Select(w=>Get(w,"Project")).FirstOrDefault(x=>x!=null&&x.GetType().FullName=="InSAT.Framework.Core.DataModel.Instances.ProjectInstance");
                if(ownedProject==null){Volatile.Write(ref done,0);return;}
                if(!string.Equals(Path.GetFullPath(Get(Get(ownedProject,"StoreProjectService"),"Location")!.ToString()!),path,StringComparison.OrdinalIgnoreCase))throw new Exception("Opened project path mismatch");
                Call(ownedProject,"RequestAllItems",true);
                Compile(ownedProject);
                Call(ownedProject,"Dispose");ownedProject=null;
            }
            if(Environment.GetEnvironmentVariable("FMB_CREATE_PROJECT")=="1"){
                var dir=Path.Combine(Path.GetTempPath(),"ms4d_fast_modbus_20261122","project",DateTime.UtcNow.ToString("yyyyMMdd_HHmmss"));
                Directory.CreateDirectory(dir);
                if(Directory.EnumerateFiles(dir,"*.fdb",SearchOption.AllDirectories).Any())throw new Exception("New-project target already exists");
                var info=Activator.CreateInstance(Find("InSAT.Framework.Core.Services.Store.ProjectInfo"),new object[]{"FastModbusBench",dir,"MasterSCADA.Project.HMI.Default","MasterPLC",DateTime.Now,"FirebirdStore"})!;
                var helper=Find("InSAT.Framework.UI.Helpers.ProjectHelper");
                var project=helper.GetMethod("GetNewProject",BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Static)!.Invoke(null,new[]{"MasterPLC",info})!;
                ownedProject=project;
                Call(project,"RequestAllItems",true);
                Log("CREATED "+Get(Get(project,"StoreProjectService"),"Location"));
                Build(project,manager);
                Call(Get(project,"StoreProjectService")!,"SaveProject",new object?[]{null});
                Log("SAVED");
                Compile(project);
                File.WriteAllText(Path.Combine(Path.GetTempPath(),"ms4d_fast_modbus_20261122","project-path.txt"),Get(Get(project,"StoreProjectService"),"Location")!.ToString());
                Call(project,"Dispose");
                ownedProject=null;
            }
            Log("INSPECTION COMPLETE");
        }catch(Exception ex){Volatile.Write(ref done,1);Log("ERROR "+ex);}
        finally{if(Volatile.Read(ref done)!=0){timer?.Dispose();if(ownedProject!=null){try{Call(ownedProject,"Dispose");}catch(Exception ex){Log("DISPOSE "+ex.Message);}ownedProject=null;}Application.Current.Shutdown();}}
    }
}


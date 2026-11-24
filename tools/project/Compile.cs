using System;
using System.Collections;
using System.Linq;
using System.Reflection;
using System.IO;
public static partial class StartupHook{
    static void ExportDatabase(object store,string path){
        var export=store.GetType().GetMethods().Single(m=>m.Name=="ExportProject"&&m.GetParameters().Length==5);
        var format=Nullable.GetUnderlyingType(export.GetParameters()[1].ParameterType)!;
        using(var stream=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.None))export.Invoke(store,new object?[]{stream,Enum.Parse(format,"Database"),null,true,false});
    }
    static object? Member(object? o,string n)=>o==null?null:Get(o,n)??o.GetType().GetField(n,BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance)?.GetValue(o);
    static object Service(object instance,string name){
        var type=Find(name);
        var method=instance.GetType().GetMethods(BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance).Single(m=>m.Name=="FindService"&&!m.IsGenericMethod&&m.GetParameters().Length==2&&m.GetParameters()[0].ParameterType==typeof(Type));
        return method.Invoke(instance,new object[]{type,true})!;
    }
    static void Compile(object p){
        var manager=Service(p,"MasterSCADA.Controllers.RT.Services.ProjectManager.ProjectManagerService");
        if(Get(manager,"ProjectMode")?.ToString()!="Stop")throw new Exception("Project must be stopped");
        var controllers=((IEnumerable)Get(manager,"Controllers")!).Cast<object>().Where(c=>Convert.ToInt64(Get(c,"Id"))>0).ToList();
        if(controllers.Count!=1)throw new Exception("Expected one isolated controller");
        var c=controllers[0];
        var cm=Service(c,"MasterSCADA.Controllers.RT.Services.ControllerManager.ControllerManagerService");
        if(Get(cm,"Session")!=null)throw new Exception("Unexpected runtime session");
        var config=Service(c,"MasterSCADA.Controllers.Services.ConfigurationService.ControllerConfigurationService");
        config.GetType().GetField("ImitationMode",BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance)!.SetValue(config,false);
        Log("Starting native configuration compilation with active dispatcher");
        var dispatcher=System.Windows.Application.Current.Dispatcher;
        var frame=new System.Windows.Threading.DispatcherFrame();
        var compile=System.Threading.Tasks.Task.Run(()=>Call(manager,"CheckProjectConfig",null,true,false));
        compile.ContinueWith(_=>dispatcher.BeginInvoke(new Action(()=>frame.Continue=false)));
        System.Windows.Threading.Dispatcher.PushFrame(frame);
        var result=compile.GetAwaiter().GetResult();
        Log("COMPILE result="+result+" hasErrors="+Get(manager,"HasErrors")+" hasWarnings="+Get(manager,"HasWarnings"));
        foreach(var e in (IEnumerable)Get(manager,"Errors")!)Log("COMPILE ERROR "+e);
        var cfg=Get(config,"CurrentConfig");
        if(cfg==null||Get(manager,"HasErrors") is true)throw new Exception("Native compilation failed");
        if(Get(cm,"Session")!=null||Get(manager,"ProjectMode")?.ToString()!="Stop")throw new Exception("Runtime unexpectedly started");
        var tasks=((IEnumerable)Member(cfg,"Tasks")!).Cast<object>().Where(t=>Member(t,"TaskType")?.ToString()=="SerialModules").ToList();
        if(tasks.Count!=1)throw new Exception("Expected exactly one protocol task");
        var proto=Member(Member(Member(tasks[0],"InterfaceOptions"),"CurrentProtocol"),"Protocol");
        if(Member(proto,"TypeID")?.ToString()!="FastModbus")throw new Exception("Wrong native protocol TypeID");
        var output=Path.Combine(Path.GetTempPath(),"ms4d_fast_modbus_20261122","native",Get(p,"ProjectId")!.ToString()!+"_"+DateTime.UtcNow.ToString("HHmmss"));
        Directory.CreateDirectory(output);
        var fbConfigs=Member(tasks[0],"FBConfigs") as IEnumerable;
        if(fbConfigs!=null){int index=0;foreach(var fb in fbConfigs){File.WriteAllText(Path.Combine(output,"fb-"+index+"-source.txt"),Member(fb,"SourceText")?.ToString());File.WriteAllText(Path.Combine(output,"fb-"+index+"-compiled.txt"),Member(fb,"Text")?.ToString());index++;}}
        using(var stream=new FileStream(Path.Combine(output,"controller.zip"),FileMode.CreateNew,FileAccess.Write,FileShare.None))Call(config,"ExportConfig",stream,true,true);
        var store=Get(p,"StoreProjectService")!;
        ExportDatabase(store,Path.Combine(output,"FastModbusBench.zip"));
        File.WriteAllText(Path.Combine(Path.GetTempPath(),"ms4d_fast_modbus_20261122","native-export-path.txt"),output);
        Log("EXPORTED "+output);
        Log("COMPILE COMPLETE: no runtime session");
    }
}

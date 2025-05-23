
idf_component_register(

    SRCS
    	"src/aes.c"
        "src/lora_funcs.c"
        "src/lora_task.c"
                
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos
    
    PRIV_REQUIRES
    	gpio sx126x
)
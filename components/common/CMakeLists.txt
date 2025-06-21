
idf_component_register(

    SRCS
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos
    
    PRIV_REQUIRES
		espnow gpio lora sx126x
)
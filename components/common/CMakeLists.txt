idf_component_register(
    SRCS
        
    INCLUDE_DIRS 
    	"include"
    
    REQUIRES
    	driver freertos nvs_flash
    
    PRIV_REQUIRES
		espnow gpio lora sx126x
)
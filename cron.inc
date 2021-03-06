<?php

// Copyright 2008-2009 Jonathan Brown


function _storage_api_ensure_all_instances_exist(&$file, $containers) {

  // check for instance of file in each container
  
  foreach($containers as $container) {
  
    if($container['suspended'] != 0 ||
      $container['service_suspended'] != 0) {
      
      $failure = TRUE;
      continue;
    }

    $service_info = _storage_api_service_invoke($container['service_id'], 'service_info');
  
    if(!(!$service_info['dynamic'] && $container['serving'] && $file['force_download'])) {

      if($file['object_id']) {
        $exists = _storage_api_object_instance_exists($file['object_id'], $container['container_id']);
        $check = FALSE;
      }
      else {
        $check = TRUE;
      }
      
      if(!$exists) {
        $filepath = storage_api_get_local_filepath($file, $keep_me);
        
        if($filepath) {
        
          if($check)
            $exists = _storage_api_object_instance_exists($file['object_id'], $container['container_id']);
          
          if(!$exists) {
            $file['filepath'] = $filepath;
            $file['keep_me'] = $keep_me;
            
            if(!_storage_api_create_object_instance($container, $file))
              $failure = TRUE;
          }
          
          if(!$keep_me)
            @unlink($filepath);
        }
        else {
          $failure = TRUE;
        }
      }
    }
    else {
      $exists = _storage_api_file_instance_exists($file['file_id'], $container['container_id']);
      
      if(!$exists) {
        $filepath = storage_api_get_local_filepath($file, $keep_me);
        
        if($filepath) {
          $file['filepath'] = $filepath;
          $file['keep_me'] = $keep_me;
        
          $failure = !_storage_api_create_file_instance($container, $file);
            
          if(!$keep_me)
            @unlink($filepath);
          }
        else {
          $failure = TRUE;
        }
      }
    }
  }
  
  return !$failure;
}


function _storage_api_destroy_object_instance($file, $container) {

  if($container['suspended'] != 0 ||
    $container['service_suspended'] != 0)
  {
    return FALSE;
  }

  $success = _storage_api_service_invoke($container['service_id'], 'object_instance_destroy', 
    $container, $file['object_id']);
  
  if(!$success)
    return FALSE;

  db_query("
    DELETE FROM {storage_object_instances}
    WHERE object_id = %d
      AND container_id = %d
  ",
    $file['object_id'],
    $container['container_id']
  );
  
  $message = 'Object instance destroyed. <br />';
  $message .= _storage_api_container_log_info($container);
  $message .= _storage_api_object_log_info($file);
  
  watchdog('storage_api', $message, NULL);

  return TRUE;
}


function _storage_api_destroy_object_instance_if_unrequired($file, $container) {
      
  $files = db_result(db_query("
    SELECT COUNT(*)
    FROM {storage_files}
    INNER JOIN {storage_selectors}
      ON {storage_files}.selector_id = {storage_selectors}.selector_id
    INNER JOIN {storage_class_containers}
      ON {storage_selectors}.class_id = {storage_class_containers}.class_id
    WHERE {storage_files}.file_id != %d
      AND {storage_files}.object_id = %d
      AND {storage_class_containers}.container_id = %d
  ",
    $file['file_id'],
    $file['object_id'],
    $container['container_id']
  ));
  
  if($files == 0) {
    $success = _storage_api_destroy_object_instance($file, $container);
    
    if(!$success)
      $failure = TRUE;
  }
  
  return !$failure;
}


function _storage_api_destroy_file_instance($file, $container) {

  if($container['suspended'] != 0 ||
    $container['service_suspended'] != 0)
  {
    return FALSE;
  }

  $success = _storage_api_service_invoke($container['service_id'], 'file_instance_destroy', 
    $container, $file['file_id']);
    
  if(!$success)
    return FALSE;

  db_query("
    DELETE FROM {storage_file_instances}
    WHERE file_id = %d
      AND container_id = %d
  ",
    $file['file_id'],
    $container['container_id']
  );
  
  $message = 'File instance destroyed' . ($file['filename'] ? (': ' . $file['filename'] . ' ') : '') . '. <br />';
  $message .= _storage_api_container_log_info($container);
  $message .= _storage_api_file_log_info($file);

  watchdog('storage_api', $message, NULL);

  return TRUE;
}


function _storage_api_destroy_unrequired_instances($file, $container_ids) {

  if(count($container_ids)) {
    $placeholders = implode(',', array_fill(0, count($container_ids), '%d'));
    $objects_and = "AND {storage_object_instances}.container_id NOT IN($placeholders)";
    $files_and = "AND {storage_file_instances}.container_id NOT IN($placeholders)";
  }
  
  
  $result = db_query("
    SELECT {storage_containers}.*, {storage_object_instances}.reference, 
      {storage_services}.suspended AS service_suspended
    FROM {storage_object_instances}
    INNER JOIN {storage_containers}
      ON {storage_object_instances}.container_id = {storage_containers}.container_id
    INNER JOIN {storage_services}
      ON {storage_containers}.service_id = {storage_services}.service_id
    WHERE {storage_object_instances}.object_id = %d
      $objects_and
    ORDER BY {storage_containers}.container_id
  ",
    array_merge(array($file['object_id']), $container_ids)
  );

  while($container = db_fetch_array($result)) {
    $container['settings'] = unserialize($container['settings']);
    $success = _storage_api_destroy_object_instance_if_unrequired($file, $container);
    
    if(!$success)
      $failure = TRUE;
  }


  $result = db_query("
    SELECT {storage_containers}.*, {storage_file_instances}.reference, 
      {storage_services}.suspended AS service_suspended
    FROM {storage_file_instances}
    INNER JOIN {storage_containers}
      ON {storage_file_instances}.container_id = {storage_containers}.container_id
    INNER JOIN {storage_services}
      ON {storage_containers}.service_id = {storage_services}.service_id
    WHERE {storage_file_instances}.file_id = %d
      $files_and
    ORDER BY {storage_containers}.container_id
  ",
    array_merge(array($file['file_id']), $container_ids)
  );  

  while($container = db_fetch_array($result)) {
    $container['settings'] = unserialize($container['settings']);
    $success = _storage_api_destroy_file_instance($file, $container);
    
    if(!$success)
      $failure = TRUE;
  }
  
  return !$failure;
}


function _storage_api_check_file($file) {

  // get all containers this file is supposed to be in

  $result = db_query("
    SELECT {storage_containers}.*, {storage_services}.suspended AS service_suspended
    FROM {storage_files}
    INNER JOIN {storage_selectors}
      ON {storage_files}.selector_id = {storage_selectors}.selector_id
    INNER JOIN {storage_classes}
      ON {storage_selectors}.class_id = {storage_classes}.class_id
    INNER JOIN {storage_class_containers}
      ON {storage_classes}.class_id = {storage_class_containers}.class_id
    INNER JOIN {storage_containers}
      ON {storage_class_containers}.container_id = {storage_containers}.container_id
    INNER JOIN {storage_services}
      ON {storage_containers}.service_id = {storage_services}.service_id
    WHERE file_id = %d
    ORDER BY {storage_class_containers}.weight, {storage_containers}.container_id
  ",
    $file['file_id']
  );
  
  $containers = array();
  $container_ids = array();
  
  while($container = db_fetch_array($result)) {
    $container['settings'] = unserialize($container['settings']);
    $containers[] = $container;
    $container_ids[] = $container['container_id'];
  }
  
  if(_storage_api_ensure_all_instances_exist($file, $containers)) {
    $success = _storage_api_destroy_unrequired_instances($file, $container_ids);
  }
  
  if($success) {
    if($file['selector_id']) {

      db_query("
        UPDATE {storage_files}
        SET check_me = 0
        WHERE file_id = %d
      ",
        $file['file_id']
      );
    }
    else {
      db_query("
        DELETE FROM {storage_files}
        WHERE file_id = %d
      ",
        $file['file_id']
      );

      _storage_api_delete_object($file['object_id']);
    }
  }
}


function _storage_api_check_files() {

  $result = db_query("
    SELECT file_id
    FROM {storage_files}
    WHERE check_me = 1
  ");
  
  while($file = db_fetch_array($result)) {
    $file = storage_api_file_load($file['file_id']);
    _storage_api_check_file($file);
  }
}


function _storage_api_purge_storage_servings() {

  db_query("
    DELETE FROM {storage_servings}
    WHERE timestamp < %d
  ",
    $_SERVER['REQUEST_TIME'] - variable_get('statistics_flush_accesslog_timer', 259200)
  );
}


function _storage_api_cron() {

  _storage_api_check_files();
  _storage_api_purge_storage_servings();
}


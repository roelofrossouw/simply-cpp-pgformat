SELECT
    CASE WHEN my_col IS NOT NULL THEN
        'k73nSqfVlf'
    ELSE
        'S0P_.Dqc84'
    END AS my_new_col,
    CASE WHEN TRIM(my_other_col) = 'UMCOtArBZK' THEN
        'k73nSqfVlf'
    ELSE
        'S0P_.Dqc84'
    END AS new_col
FROM
    my_table
WHERE
    col1 = 123
    AND col2 = 56
    AND col3 = 'xSEHPeFrQi';

